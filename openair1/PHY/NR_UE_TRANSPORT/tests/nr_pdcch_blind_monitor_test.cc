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
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/*! \file openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_blind_monitor_test.cc
 * \brief Phase 3 (TOTAL_PASSIVE_UE_HANDOVER.md): offline synthetic-DCI-injection test for blind
 * PDCCH/DCI-1_1 decode + extract, against known ground truth -- required by this project's own
 * testing plan before any live-air attempt ("do not blind-search real air on unverified decode
 * logic"). See /home/sens/.claude/plans/zesty-baking-thompson.md for the design record.
 *
 * Ground truth is generated the same way this repo's own polar coding testbench
 * (openair1/PHY/CODING/TESTBENCH/polartest.c) does: polar_encoder_dci() -> BPSK -> AWGN -> int16
 * LLR -> polar_decoder_int16(). Group 0 (RawPayloadRoundTrip) verifies PackPayload()'s bit
 * convention empirically against the real encoder/decoder BEFORE any other test trusts it --
 * deliberately not just assumed, since it was derived by reading nr_ue_procedures.c's readBits()
 * convention rather than by an independent worked example.
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

extern "C" {
#include "PHY/CODING/nrPolar_tools/nr_polar_dci_defs.h"
#include "PHY/CODING/nrPolar_tools/nr_polar_defs.h"
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
#include "common/utils/nr/nr_common.h"
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h" // NR_tda_info_t, get_dl_tda_info(), TYPE_C_RNTI_
#include "nr_pdcch_blind_monitor.h"
#include "nr_pdcch_blind_monitor_rt.h"
#include "executables/softmodem-common.h"
}

// nr_mac_common.c's object file (needed here for get_dl_tda_info()/fill_dmrs_mask()) also defines
// get_pusch_nb_antenna_ports(), which this test never calls but which references
// get_softmodem_params() -- pulling that in for real means linking executables/softmodem-common.c,
// a heavy dependency this lean offline test has no other reason to need. Stubbed directly in this
// translation unit, same "sidestep static-archive link-order fragility" convention
// isac_sync_test.cc already uses for uniqCfg/exit_function.
extern "C" softmodem_params_t* get_softmodem_params(void)
{
  static softmodem_params_t p;
  return &p;
}

// LOG/CONFIG_LIB need these outside a full softmodem executable -- same convention as
// openair1/PHY/NR_UE_ISAC/tests/isac_sync_test.cc.
extern "C" configmodule_interface_t* uniqCfg = nullptr;
extern "C" void exit_function(const char* file, const char* function, const int line, const char* s, const int assert)
{
  if (s != nullptr) {
    fprintf(stderr, "%s:%d %s() Exiting: %s\n", file, line, function, s);
  }
  if (assert) {
    abort();
  } else {
    exit(EXIT_SUCCESS);
  }
}

namespace {

constexpr uint8_t kAggregationLevel = 8; // matches polartest.c's own DCI example
// dmrs-TypeA-Position pos2 -- ASN.1 enum value confirmed 0 by this codebase's established
// sequential-from-0 asn1c convention (already relied on elsewhere, e.g. csirs_monitor's
// freq_density field: NOTHING=0,dot5=1,one=2,three=3). Passed as a plain uint8_t (the module's
// own signature), so this test never needs the ASN.1 header itself.
constexpr uint8_t kDmrsTypeAPositionPos2 = 0;

int RivBitsFor(uint16_t bwp_size)
{
  return (int)std::ceil(std::log2(((double)bwp_size * (bwp_size + 1)) / 2.0));
}

/// One synthetic DCI-1_1 payload's ground truth.
struct GroundTruth {
  uint16_t rnti               = 0x1234;
  uint16_t bwp_size           = 106;
  uint32_t riv                = 0;
  uint32_t time_domain_assignment = 0;
  uint32_t antenna_ports      = 0; // index into Table 7.3.1.2.2-1, 0..11 valid
  uint8_t  dmrs_seq_init      = 0;
  // Transport-block fields: read and discarded before 2026-07-30, now surfaced for the passive
  // data-aided decode. mcs stays < 28 by default so the reserved-range plausibility check passes.
  uint32_t mcs                = 0;
  uint32_t ndi                = 0;
  uint32_t rv                 = 0;
  uint32_t harq_pid           = 0;
};

/// Resolve one overridable field width the same way nr_pdcch_blind_monitor.c's blind_field_bits()
/// does, so a packed test payload always matches the layout the extractor will read.
int PickBits(int override_val, int dflt) { return override_val >= 0 ? override_val : dflt; }

/// Time-domain-assignment width: DERIVED from the configured TDRA entry count, exactly as
/// nr_dci_size() derives it from the real list -- never an independent knob.
int TdaBits(const nr_pdcch_blind_extract_opts_t* opts)
{
  if (opts == nullptr || opts->tda_count <= 0) return 4;
  int b = 0;
  while ((1 << b) < opts->tda_count) b++;
  return b;
}

/// Packs a GroundTruth's fields into the MSB-first, spec-field-order 64-bit layout
/// nr_pdcch_blind_decode_and_extract()'s read_field() expects (mirrors
/// nr_ue_procedures.c's readBits()/EXTRACT_DCI_ITEM convention -- see the module's file-level
/// comment). Field list and widths must match nr_pdcch_blind_monitor.c's extraction order exactly,
/// which is why `opts` is threaded through: once a width is overridden, the packer must move with it.
uint64_t PackPayload(const GroundTruth& gt, int riv_bits, const nr_pdcch_blind_extract_opts_t* opts = nullptr)
{
  uint64_t p = 0;
  auto put = [&](uint32_t val, int nbits) {
    if (nbits == 0) return;
    const uint32_t mask = (nbits >= 32) ? 0xFFFFFFFFu : ((1u << nbits) - 1u);
    p = (p << nbits) | (val & mask);
  };
  put(1, 1);                          // format indicator = 1 (DL)
  put(0, 0);                          // carrier indicator
  put(0, opts ? PickBits(opts->bwp_indicator_bits, 1) : 1);   // bwp indicator
  put(gt.riv, riv_bits);              // freq domain assignment
  put(gt.time_domain_assignment, TdaBits(opts));              // time domain assignment
  put(0, opts ? PickBits(opts->vrb_to_prb_bits, 0) : 0);      // vrb-to-prb mapping
  put(0, opts ? PickBits(opts->prb_bundling_bits, 0) : 0);    // prb bundling size indicator
  put(0, opts ? PickBits(opts->rate_matching_bits, 0) : 0);   // rate matching indicator
  put(0, opts ? PickBits(opts->zp_csirs_bits, 0) : 0);        // zp csi-rs trigger
  put(gt.mcs, 5);                     // MCS
  put(gt.ndi, 1);                     // NDI
  put(gt.rv, 2);                      // RV
  put(0, opts ? PickBits(opts->tb2_bits, 0) : 0);             // TB2
  put(gt.harq_pid, opts ? PickBits(opts->harq_pid_bits, 4) : 4);        // harq process number
  put(0, opts ? PickBits(opts->dai_bits, 2) : 2);             // DAI
  put(0, 2);                          // TPC PUCCH
  put(0, 3);                          // PUCCH resource indicator
  put(0, opts ? PickBits(opts->pdsch_to_harq_bits, 3) : 3);   // PDSCH-to-HARQ timing indicator
  put(gt.antenna_ports, opts ? PickBits(opts->antenna_ports_bits, 4) : 4); // antenna ports
  put(0, opts ? PickBits(opts->tci_bits, 0) : 0);             // TCI
  put(0, opts ? PickBits(opts->srs_request_bits, 2) : 2);     // SRS request
  put(0, opts ? PickBits(opts->cbg_bits, 0) : 0);             // CBGTI + CBGFI
  put(gt.dmrs_seq_init, 1);           // DMRS sequence initialization
  return p;
}

/// An all-defaults opts, i.e. every width left at the module's built-in assumption. Tests that only
/// want to exercise the TDRA/DM-RS overrides start from this so the PAYLOAD LAYOUT stays the
/// legacy one and only the thing under test changes.
nr_pdcch_blind_extract_opts_t DefaultOpts()
{
  nr_pdcch_blind_extract_opts_t o = {};
  o.dmrs_add_pos        = -1;
  o.dmrs_max_length     = 0;
  o.bwp_indicator_bits  = -1;
  o.vrb_to_prb_bits     = -1;
  o.prb_bundling_bits   = -1;
  o.rate_matching_bits  = -1;
  o.zp_csirs_bits       = -1;
  o.tb2_bits            = -1;
  o.harq_pid_bits       = -1;
  o.dai_bits            = -1;
  o.pdsch_to_harq_bits  = -1;
  o.antenna_ports_bits  = -1;
  o.tci_bits            = -1;
  o.srs_request_bits    = -1;
  o.cbg_bits            = -1;
  return o;
}

/// Encode a payload for `rnti`, BPSK-modulate, add AWGN at `snr_db`, return int16 LLRs sized to
/// the real coded length (nr_polar_params()->encoderLength). snr_db large (e.g. 40) is effectively
/// noiseless.
///
/// Uses polar_encoder_fast(), NOT polar_encoder_dci() -- openair1/PHY/NR_TRANSPORT/nr_dci.c's
/// nr_generate_dci() (the REAL gNB PDCCH encode path) calls polar_encoder_fast() with
/// `(uint64_t*)dci_pdu->Payload` directly, the exact same uint64_t[]-payload shape
/// polar_decoder_int16() produces as dci_estimation[] and nr_ue_procedures.c's readBits() consumes
/// -- no uint32_t-array bit-unpacking indirection, so no separate convention to get wrong.
/// polar_encoder_dci() is ONLY ever called from the stale, disabled-by-default
/// DEBUG_DCI_POLAR_PARAMS block in polartest.c (confirmed by grep -- not live-path code), and its
/// `in[2]` uint32_t-array convention does NOT match this (Group 0/RawPayloadRoundTrip caught the
/// mismatch empirically before this fix).
std::vector<int16_t> EncodeToLLR(uint64_t payload, uint16_t rnti, uint16_t dci_length, uint8_t agg_level, double snr_db,
                                 std::mt19937& rng)
{
  t_nrPolar_params* params      = nr_polar_params(NR_POLAR_DCI_MESSAGE_TYPE, dci_length, agg_level);
  const uint16_t    encoder_len = params->encoderLength;

  std::vector<uint32_t> out((encoder_len + 31) / 32, 0);
  polar_encoder_fast(&payload, out.data(), (int32_t)rnti, /*ones_flag=*/1, NR_POLAR_DCI_MESSAGE_TYPE, dci_length,
                     agg_level);

  std::vector<uint8_t> coded_bits(encoder_len);
  nr_bit2byte_uint32_8(out.data(), encoder_len, coded_bits.data());

  const double snr_lin     = std::pow(10.0, snr_db / 10.0);
  const double noise_sigma = 1.0 / std::sqrt(2.0 * snr_lin);
  std::normal_distribution<double> noise(0.0, noise_sigma);

  // Scale/clip EXACTLY matching polartest.c's own decoder_int16 path (its only int16-LLR example
  // that isn't the stale DEBUG_DCI_POLAR_PARAMS block): channelOutput_int16[i] = clip(8*rx, -128,
  // 127). This is load-bearing, not cosmetic -- an earlier, more aggressive scale (rx*8192, clipped
  // to +-32000) made even a noiseless all-zero-payload round trip fail outright (confirmed via a
  // standalone diagnostic isolating polar_encoder_fast+polar_decoder_int16 alone): the list
  // decoder's internal metric arithmetic evidently assumes LLR magnitudes in this modest range, not
  // the full int16 span.
  std::vector<int16_t> llr(encoder_len);
  for (int i = 0; i < encoder_len; i++) {
    const double bpsk = (coded_bits[i] == 0) ? (1.0 / std::sqrt(2.0)) : (-1.0 / std::sqrt(2.0));
    const double rx   = bpsk + noise(rng);
    double       v    = rx * 8.0;
    v                  = std::max(-128.0, std::min(127.0, v));
    llr[i]             = (int16_t)v;
  }
  return llr;
}

class BlindPdcchTest : public ::testing::Test {
protected:
  void SetUp() override { rng_.seed(12345); }
  std::mt19937 rng_;
};

// ---------------------------------------------------------------------------------------------
// Group 0: raw payload round-trip. MUST pass before any field-level assertion elsewhere in this
// file is meaningful -- see the file-level comment.
// ---------------------------------------------------------------------------------------------
TEST_F(BlindPdcchTest, RawPayloadRoundTrip) {
  const uint16_t bwp_size   = 106;
  const int      riv_bits   = RivBitsFor(bwp_size);
  const uint16_t dci_length = nr_pdcch_blind_dci_size(bwp_size);

  GroundTruth gt;
  gt.rnti                   = 0x4601;
  gt.bwp_size                = bwp_size;
  gt.riv                     = 12345u % (1u << riv_bits);
  gt.time_domain_assignment  = 3;
  gt.antenna_ports            = 5;
  gt.dmrs_seq_init            = 1;

  const uint64_t packed = PackPayload(gt, riv_bits);
  auto llr = EncodeToLLR(packed, gt.rnti, dci_length, kAggregationLevel, /*snr_db=*/40.0, rng_);

  uint64_t dci_estimation[2] = {0};
  const uint32_t crc =
      polar_decoder_int16(llr.data(), dci_estimation, 1, NR_POLAR_DCI_MESSAGE_TYPE, dci_length, kAggregationLevel);

  ASSERT_EQ(crc, gt.rnti) << "RNTI must round-trip before any payload-bit assertion is meaningful";
  ASSERT_EQ(dci_estimation[0], packed)
      << "PackPayload()'s bit convention does not match polar_encoder_dci/polar_decoder_int16's "
         "actual layout -- fix PackPayload() (and re-check read_field() in "
         "nr_pdcch_blind_monitor.c against whatever mismatch this reveals) before trusting any "
         "other test in this file";
}

// ---------------------------------------------------------------------------------------------
// Group 1: DCI size/layout.
// ---------------------------------------------------------------------------------------------
TEST(DciSize, MatchesHandDerivedValues) {
  // 106 PRB (rfsim harness): riv_span=106*107/2=5671, ceil(log2(5671))=13, total=35+13=48.
  EXPECT_EQ(nr_pdcch_blind_dci_size(106), 48);
  // 51 PRB (this project's live OTA cell, per CLAUDE.md): riv_span=51*52/2=1326,
  // ceil(log2(1326))=11, total=35+11=46.
  EXPECT_EQ(nr_pdcch_blind_dci_size(51), 46);
}

TEST(DciSize, ZeroBwpIsInvalid) {
  EXPECT_EQ(nr_pdcch_blind_dci_size(0), 0);
}

// The decomposition of the long-standing "dci_length_override is 3 bits below the formula" gap,
// pinned as a test because it is the whole reason the per-field widths exist. This project's gNB:
// no additional DL BWP (bwp_indicator 1->0) and a 3-entry pdsch-TimeDomainAllocationList
// (time-domain assignment 4->2). Both totals below are LIVE-VERIFIED payload lengths, so a change
// that breaks either one is a real regression, not a cosmetic disagreement.
TEST(DciSize, DeploymentFieldWidthsReproduceTheLiveVerifiedLengths) {
  nr_pdcch_blind_extract_opts_t opts = DefaultOpts();
  opts.tda_count      = 3; // -> ceil(log2(3)) = 2 bits
  opts.tda_start[0]   = 1; opts.tda_length[0] = 13;
  opts.tda_start[1]   = 1; opts.tda_length[1] = 12;
  opts.tda_start[2]   = 1; opts.tda_length[2] = 5;
  opts.dmrs_add_pos   = 1;
  opts.dmrs_max_length = 1;
  opts.bwp_indicator_bits = 0;

  EXPECT_EQ(nr_pdcch_blind_dci_size_ex(106, &opts), 45); // live-verified 2026-07-28
  EXPECT_EQ(nr_pdcch_blind_dci_size_ex(273, &opts), 48); // live-verified 2026-07-28 off the gNB log

  // With no overrides it must reproduce the plain formula exactly -- the "opt-in changes nothing"
  // guarantee, at the size level.
  EXPECT_EQ(nr_pdcch_blind_dci_size_ex(106, nullptr), nr_pdcch_blind_dci_size(106));
  EXPECT_EQ(nr_pdcch_blind_dci_size_ex(273, nullptr), nr_pdcch_blind_dci_size(273));
  EXPECT_EQ(nr_pdcch_blind_dci_size_ex(51, nullptr), nr_pdcch_blind_dci_size(51));
}

// ---------------------------------------------------------------------------------------------
// Group 2: CRC/RNTI recovery.
// ---------------------------------------------------------------------------------------------
TEST_F(BlindPdcchTest, RecoversInRangeRntiAtHighSnr) {
  const uint16_t bwp_size   = 106;
  const int      riv_bits   = RivBitsFor(bwp_size);
  const uint16_t dci_length = nr_pdcch_blind_dci_size(bwp_size);

  GroundTruth gt;
  gt.rnti                   = 0x1234;
  gt.bwp_size                = bwp_size;
  gt.riv                     = (uint32_t)PRBalloc_to_locationandbandwidth0(20, 10, bwp_size);
  gt.time_domain_assignment  = 1;
  gt.antenna_ports            = 0;
  gt.dmrs_seq_init            = 0;

  const uint64_t packed = PackPayload(gt, riv_bits);

  for (const double snr_db : {20.0, 10.0, 6.0}) {
    auto llr = EncodeToLLR(packed, gt.rnti, dci_length, kAggregationLevel, snr_db, rng_);
    nr_pdcch_blind_result_t out;
    const bool ok = nr_pdcch_blind_decode_and_extract(llr.data(), kAggregationLevel, dci_length, bwp_size,
                                                       kDmrsTypeAPositionPos2, NR_PDCCH_BLIND_RNTI_MIN_DEFAULT,
                                                       NR_PDCCH_BLIND_RNTI_MAX_DEFAULT, &out);
    EXPECT_TRUE(ok) << "SNR " << snr_db << " dB: " << (out.reject_reason ? out.reject_reason : "?");
    EXPECT_EQ(out.rnti, gt.rnti) << "SNR " << snr_db << " dB";
  }
}

TEST_F(BlindPdcchTest, RejectsOutOfRangeRntiEvenNoiseless) {
  const uint16_t bwp_size   = 106;
  const int      riv_bits   = RivBitsFor(bwp_size);
  const uint16_t dci_length = nr_pdcch_blind_dci_size(bwp_size);

  GroundTruth gt;
  // NR_PDCCH_BLIND_RNTI_MAX_DEFAULT is 0xFFEF; pick something just past it (still <0xFFFF=SI-RNTI,
  // which is reserved separately) -- the exact boundary a naive equality->range-check port could
  // get backwards (off-by-one at the range edge).
  gt.rnti                   = NR_PDCCH_BLIND_RNTI_MAX_DEFAULT + 1;
  gt.bwp_size                = bwp_size;
  gt.riv                     = (uint32_t)PRBalloc_to_locationandbandwidth0(20, 10, bwp_size);
  gt.time_domain_assignment  = 1;
  gt.antenna_ports            = 0;
  gt.dmrs_seq_init            = 0;

  const uint64_t packed = PackPayload(gt, riv_bits);
  auto llr = EncodeToLLR(packed, gt.rnti, dci_length, kAggregationLevel, /*snr_db=*/40.0, rng_);

  nr_pdcch_blind_result_t out;
  const bool ok = nr_pdcch_blind_decode_and_extract(llr.data(), kAggregationLevel, dci_length, bwp_size,
                                                     kDmrsTypeAPositionPos2, NR_PDCCH_BLIND_RNTI_MIN_DEFAULT,
                                                     NR_PDCCH_BLIND_RNTI_MAX_DEFAULT, &out);
  EXPECT_FALSE(ok);
  EXPECT_FALSE(out.plausible);
  ASSERT_NE(out.reject_reason, nullptr);
  EXPECT_STREQ(out.reject_reason, "CRC-recovered value outside plausible RNTI range");
}

// ---------------------------------------------------------------------------------------------
// Group 3: field extraction.
// ---------------------------------------------------------------------------------------------
TEST_F(BlindPdcchTest, ExtractsAllFieldsCorrectly) {
  const uint16_t bwp_size   = 106;
  const int      riv_bits   = RivBitsFor(bwp_size);
  const uint16_t dci_length = nr_pdcch_blind_dci_size(bwp_size);

  const uint16_t expected_start_rb = 15;
  const uint16_t expected_num_rb   = 30;

  GroundTruth gt;
  gt.rnti                   = 0x2AF0;
  gt.bwp_size                = bwp_size;
  gt.riv = (uint32_t)PRBalloc_to_locationandbandwidth0(expected_num_rb, expected_start_rb, bwp_size);
  gt.time_domain_assignment  = 2;
  gt.antenna_ports            = 9; // g_table_7_3_2_3_3_1 row 9 = {2,1,1,1,0}
  gt.dmrs_seq_init            = 1;

  const uint64_t packed = PackPayload(gt, riv_bits);
  auto llr = EncodeToLLR(packed, gt.rnti, dci_length, kAggregationLevel, /*snr_db=*/40.0, rng_);

  nr_pdcch_blind_result_t out;
  const bool ok = nr_pdcch_blind_decode_and_extract(llr.data(), kAggregationLevel, dci_length, bwp_size,
                                                     kDmrsTypeAPositionPos2, NR_PDCCH_BLIND_RNTI_MIN_DEFAULT,
                                                     NR_PDCCH_BLIND_RNTI_MAX_DEFAULT, &out);
  ASSERT_TRUE(ok) << (out.reject_reason ? out.reject_reason : "?");

  EXPECT_EQ(out.rnti, gt.rnti);
  EXPECT_EQ(out.start_rb, expected_start_rb);
  EXPECT_EQ(out.num_rb, expected_num_rb);

  // Independently call the same reused extraction primitive to compute the expected TDA fields --
  // this test's job is confirming the MODULE correctly threads the decoded index through to it,
  // not re-validating get_dl_tda_info()'s own table content (already relied on elsewhere in this
  // codebase).
  const NR_tda_info_t expected_tda =
      get_dl_tda_info(nullptr, 0, (int)gt.time_domain_assignment, kDmrsTypeAPositionPos2, 1, TYPE_C_RNTI_, 0, false);
  ASSERT_TRUE(expected_tda.valid_tda);
  EXPECT_EQ(out.start_symbol, expected_tda.startSymbolIndex);
  EXPECT_EQ(out.num_symbols, expected_tda.nrOfSymbols);

  // Table 7.3.1.2.2-1 row 9 = {n_cdm_groups=2, port0=1, port1=1, port2=1, port3=0}
  EXPECT_EQ(out.n_dmrs_cdm_groups, 2);
  EXPECT_EQ(out.dmrs_ports, 0x7u); // ports 0,1,2 active -> bits 0,1,2 set = 0b0111
  EXPECT_EQ(out.nscid, gt.dmrs_seq_init);
}

// ---------------------------------------------------------------------------------------------
// Group 3b: transport-block fields + deployment overrides (2026-07-30). These exist ONLY because
// the passive data-aided decode needs them; nothing before this session read past the allocation.
// ---------------------------------------------------------------------------------------------
TEST_F(BlindPdcchTest, ExtractsTransportBlockFields) {
  const uint16_t bwp_size   = 106;
  const int      riv_bits   = RivBitsFor(bwp_size);
  const uint16_t dci_length = nr_pdcch_blind_dci_size(bwp_size);

  GroundTruth gt;
  gt.rnti                   = 0x4711;
  gt.bwp_size                = bwp_size;
  gt.riv                     = (uint32_t)PRBalloc_to_locationandbandwidth0(24, 8, bwp_size);
  gt.time_domain_assignment  = 1;
  gt.antenna_ports            = 0;
  gt.dmrs_seq_init            = 0;
  // Deliberately all-distinct and non-zero: MCS/NDI/RV/HARQ-pid are ADJACENT fields, so a
  // one-bit-off read_field() offset would swap or shift them, and a test using zeros everywhere
  // could not see that.
  gt.mcs                      = 19;
  gt.ndi                      = 1;
  gt.rv                       = 2;
  gt.harq_pid                 = 11;

  const uint64_t packed = PackPayload(gt, riv_bits);
  auto llr = EncodeToLLR(packed, gt.rnti, dci_length, kAggregationLevel, /*snr_db=*/40.0, rng_);

  nr_pdcch_blind_result_t out;
  const bool ok = nr_pdcch_blind_decode_and_extract(llr.data(), kAggregationLevel, dci_length, bwp_size,
                                                     kDmrsTypeAPositionPos2, NR_PDCCH_BLIND_RNTI_MIN_DEFAULT,
                                                     NR_PDCCH_BLIND_RNTI_MAX_DEFAULT, &out);
  ASSERT_TRUE(ok) << (out.reject_reason ? out.reject_reason : "?");
  EXPECT_EQ(out.mcs, gt.mcs);
  EXPECT_EQ(out.ndi, gt.ndi);
  EXPECT_EQ(out.rv, gt.rv);
  EXPECT_EQ(out.harq_pid, gt.harq_pid);
  EXPECT_EQ(out.tda_index, gt.time_domain_assignment);
  EXPECT_EQ(out.mapping_type, 0); // default TDRA index 1 is mapping type A
}

TEST_F(BlindPdcchTest, TdaOverrideReplacesTheDefaultTable) {
  const uint16_t bwp_size   = 106;
  const int      riv_bits   = RivBitsFor(bwp_size);
  const uint16_t dci_length = nr_pdcch_blind_dci_size(bwp_size);

  GroundTruth gt;
  gt.rnti                   = 0x51A0;
  gt.bwp_size                = bwp_size;
  gt.riv                     = (uint32_t)PRBalloc_to_locationandbandwidth0(20, 10, bwp_size);
  gt.time_domain_assignment  = 0;
  gt.antenna_ports            = 0;

  const uint64_t packed = PackPayload(gt, riv_bits);
  auto llr = EncodeToLLR(packed, gt.rnti, dci_length, kAggregationLevel, /*snr_db=*/40.0, rng_);

  // Baseline: the spec default table's index 0 is S=2, L=12.
  nr_pdcch_blind_result_t def;
  ASSERT_TRUE(nr_pdcch_blind_decode_and_extract(llr.data(), kAggregationLevel, dci_length, bwp_size,
                                                kDmrsTypeAPositionPos2, NR_PDCCH_BLIND_RNTI_MIN_DEFAULT,
                                                NR_PDCCH_BLIND_RNTI_MAX_DEFAULT, &def));
  EXPECT_EQ(def.start_symbol, 2);
  EXPECT_EQ(def.num_symbols, 12);

  // This project's gNB installs its own list instead (nr_rrc_config_dl_tda(): index 0 =
  // S=len_coreset, L=14-len_coreset, i.e. S=1/L=13 for a 1-symbol CORESET). The whole reason the
  // override exists is that this difference is INVISIBLE to a DM-RS-only tap -- the front-loaded
  // DM-RS symbol is at l0=2 either way -- and fatal to a decode.
  nr_pdcch_blind_extract_opts_t opts = DefaultOpts();
  opts.tda_count      = 3;
  opts.tda_start[0]   = 1;  opts.tda_length[0] = 13;
  opts.tda_start[1]   = 1;  opts.tda_length[1] = 12;
  opts.tda_start[2]   = 1;  opts.tda_length[2] = 5;

  // A 3-entry TDRA list narrows the time-domain-assignment field to 2 bits, so the payload has a
  // DIFFERENT layout and must be re-packed -- re-using the 4-bit-TDA `llr` above would be testing a
  // deliberately mismatched pair.
  const uint16_t ovr_len = nr_pdcch_blind_dci_size_ex(bwp_size, &opts);
  ASSERT_EQ(ovr_len, dci_length - 2) << "a 3-entry TDRA list should narrow the payload by exactly 2 bits";
  auto llr_ovr = EncodeToLLR(PackPayload(gt, riv_bits, &opts), gt.rnti, ovr_len, kAggregationLevel, 40.0, rng_);
  nr_pdcch_blind_result_t ovr;
  ASSERT_TRUE(nr_pdcch_blind_decode_and_extract_ex(llr_ovr.data(), kAggregationLevel, ovr_len, bwp_size,
                                                   kDmrsTypeAPositionPos2, NR_PDCCH_BLIND_RNTI_MIN_DEFAULT,
                                                   NR_PDCCH_BLIND_RNTI_MAX_DEFAULT, &opts, &ovr));
  EXPECT_EQ(ovr.start_symbol, 1);
  EXPECT_EQ(ovr.num_symbols, 13);
  // Front-loaded DM-RS lands on symbol 2 in BOTH cases -- asserted so the "why this was invisible"
  // claim above is a checked fact, not a comment.
  EXPECT_TRUE(def.dl_dmrs_symb_pos & (1u << 2));
  EXPECT_TRUE(ovr.dl_dmrs_symb_pos & (1u << 2));

  // An index past the configured list must be rejected rather than silently read out of bounds.
  // Index 3 IS representable in the 2 bits a 3-entry list gets, but has no entry -- so a garbage or
  // mis-decoded candidate can land there and must be rejected, not read out of bounds.
  GroundTruth gt2 = gt;
  gt2.time_domain_assignment = 3;
  const uint16_t ovr_len2 = nr_pdcch_blind_dci_size_ex(bwp_size, &opts);
  auto llr2 = EncodeToLLR(PackPayload(gt2, riv_bits, &opts), gt2.rnti, ovr_len2, kAggregationLevel, 40.0, rng_);
  nr_pdcch_blind_result_t oob;
  EXPECT_FALSE(nr_pdcch_blind_decode_and_extract_ex(llr2.data(), kAggregationLevel, ovr_len2, bwp_size,
                                                    kDmrsTypeAPositionPos2, NR_PDCCH_BLIND_RNTI_MIN_DEFAULT,
                                                    NR_PDCCH_BLIND_RNTI_MAX_DEFAULT, &opts, &oob));
  ASSERT_NE(oob.reject_reason, nullptr);
  EXPECT_STREQ(oob.reject_reason, "time_domain_assignment index beyond the configured TDRA list");
}

TEST_F(BlindPdcchTest, DmrsAdditionalPositionChangesTheSymbolMask) {
  const uint16_t bwp_size   = 106;
  const int      riv_bits   = RivBitsFor(bwp_size);
  const uint16_t dci_length = nr_pdcch_blind_dci_size(bwp_size);

  GroundTruth gt;
  gt.rnti                   = 0x6C2D;
  gt.bwp_size                = bwp_size;
  gt.riv                     = (uint32_t)PRBalloc_to_locationandbandwidth0(20, 10, bwp_size);
  gt.time_domain_assignment  = 0;
  gt.antenna_ports            = 0;

  nr_pdcch_blind_extract_opts_t opts = DefaultOpts();
  opts.tda_count      = 1;  // -> 0 TDA bits, so the payload is 4 bits shorter than the default layout
  opts.tda_start[0]   = 1;
  opts.tda_length[0]  = 13; // ld = 14: table row where pos1 and pos2 genuinely differ
  opts.dmrs_max_length = 1;
  const uint16_t ovr_len = nr_pdcch_blind_dci_size_ex(bwp_size, &opts);
  auto llr = EncodeToLLR(PackPayload(gt, riv_bits, &opts), gt.rnti, ovr_len, kAggregationLevel, 40.0, rng_);

  // pos1 (this gNB's configured value, nr_radio_config.c:1743) vs the pos2 fallback
  // fill_dmrs_mask() assumes with no dedicated pdsch_Config.
  opts.dmrs_add_pos = 1;
  nr_pdcch_blind_result_t p1;
  ASSERT_TRUE(nr_pdcch_blind_decode_and_extract_ex(llr.data(), kAggregationLevel, ovr_len, bwp_size,
                                                   kDmrsTypeAPositionPos2, NR_PDCCH_BLIND_RNTI_MIN_DEFAULT,
                                                   NR_PDCCH_BLIND_RNTI_MAX_DEFAULT, &opts, &p1));
  opts.dmrs_add_pos = 2;
  nr_pdcch_blind_result_t p2;
  ASSERT_TRUE(nr_pdcch_blind_decode_and_extract_ex(llr.data(), kAggregationLevel, ovr_len, bwp_size,
                                                   kDmrsTypeAPositionPos2, NR_PDCCH_BLIND_RNTI_MIN_DEFAULT,
                                                   NR_PDCCH_BLIND_RNTI_MAX_DEFAULT, &opts, &p2));

  // Same front-loaded symbol, different ADDITIONAL symbols -- exactly the failure mode that makes a
  // decode fail while leaving a DM-RS-only CFR tap looking perfectly healthy.
  EXPECT_TRUE(p1.dl_dmrs_symb_pos & (1u << 2));
  EXPECT_TRUE(p2.dl_dmrs_symb_pos & (1u << 2));
  EXPECT_NE(p1.dl_dmrs_symb_pos, p2.dl_dmrs_symb_pos);
  // And pos2 must place strictly more DM-RS symbols than pos1 (2 additional vs 1).
  EXPECT_GT(__builtin_popcount((unsigned)p2.dl_dmrs_symb_pos), __builtin_popcount((unsigned)p1.dl_dmrs_symb_pos));

  // NULL opts must stay bit-identical to the plain entry point -- the "opt-in changes nothing until
  // configured" guarantee the whole override rests on. Uses the DEFAULT-layout payload.
  auto llr_def = EncodeToLLR(PackPayload(gt, riv_bits), gt.rnti, dci_length, kAggregationLevel, 40.0, rng_);
  nr_pdcch_blind_result_t plain, null_opts;
  ASSERT_TRUE(nr_pdcch_blind_decode_and_extract(llr_def.data(), kAggregationLevel, dci_length, bwp_size,
                                                kDmrsTypeAPositionPos2, NR_PDCCH_BLIND_RNTI_MIN_DEFAULT,
                                                NR_PDCCH_BLIND_RNTI_MAX_DEFAULT, &plain));
  ASSERT_TRUE(nr_pdcch_blind_decode_and_extract_ex(llr_def.data(), kAggregationLevel, dci_length, bwp_size,
                                                   kDmrsTypeAPositionPos2, NR_PDCCH_BLIND_RNTI_MIN_DEFAULT,
                                                   NR_PDCCH_BLIND_RNTI_MAX_DEFAULT, nullptr, &null_opts));
  EXPECT_EQ(plain.start_symbol, null_opts.start_symbol);
  EXPECT_EQ(plain.num_symbols, null_opts.num_symbols);
  EXPECT_EQ(plain.dl_dmrs_symb_pos, null_opts.dl_dmrs_symb_pos);
}

TEST_F(BlindPdcchTest, RejectsValidCrcWithOutOfBoundAntennaPorts) {
  // The genuinely novel case: CRC/RNTI check passes cleanly (real, well-formed, in-range-RNTI
  // encode), but a field is out of its valid bound -- the existing own-RNTI PDCCH decode path has
  // never needed to handle this (a CRC match alone is trusted there).
  const uint16_t bwp_size   = 106;
  const int      riv_bits   = RivBitsFor(bwp_size);
  const uint16_t dci_length = nr_pdcch_blind_dci_size(bwp_size);

  GroundTruth gt;
  gt.rnti                   = 0x3B3B;
  gt.bwp_size                = bwp_size;
  gt.riv                     = (uint32_t)PRBalloc_to_locationandbandwidth0(20, 10, bwp_size);
  gt.time_domain_assignment  = 1;
  gt.antenna_ports            = 14; // >= 12: reserved, outside Table 7.3.1.2.2-1
  gt.dmrs_seq_init            = 0;

  const uint64_t packed = PackPayload(gt, riv_bits);
  auto llr = EncodeToLLR(packed, gt.rnti, dci_length, kAggregationLevel, /*snr_db=*/40.0, rng_);

  nr_pdcch_blind_result_t out;
  const bool ok = nr_pdcch_blind_decode_and_extract(llr.data(), kAggregationLevel, dci_length, bwp_size,
                                                     kDmrsTypeAPositionPos2, NR_PDCCH_BLIND_RNTI_MIN_DEFAULT,
                                                     NR_PDCCH_BLIND_RNTI_MAX_DEFAULT, &out);
  EXPECT_FALSE(ok);
  EXPECT_FALSE(out.plausible);
  // The RNTI itself must still have been recovered correctly (proving the CRC check genuinely
  // passed and this rejection came from the FIELD-bound check, not a coincidental RNTI failure).
  EXPECT_EQ(out.rnti, gt.rnti);
  ASSERT_NE(out.reject_reason, nullptr);
  EXPECT_STREQ(out.reject_reason, "antenna_ports field outside Table 7.3.1.2.2-1's 12 valid rows");
}

// ---------------------------------------------------------------------------------------------
// Group 4: plausibility filter under noise. Converts the false-positive risk into a
// regression-guarded number (per this project's own testing-plan requirement) rather than an
// unmeasured assumption.
// ---------------------------------------------------------------------------------------------
TEST_F(BlindPdcchTest, PureNoiseFalseAcceptRateIsBounded) {
  const uint16_t bwp_size   = 106;
  const uint16_t dci_length = nr_pdcch_blind_dci_size(bwp_size);

  t_nrPolar_params* params      = nr_polar_params(NR_POLAR_DCI_MESSAGE_TYPE, dci_length, kAggregationLevel);
  const uint16_t    encoder_len = params->encoderLength;

  constexpr int kTrials = 3000;
  int           accepted = 0;
  // Range matches EncodeToLLR()'s real clipped LLR range (-128..127, see its own comment) -- real
  // int16 LLR values from this decoder's actual usage never exceed that, so a noise model outside
  // it would not be representative of production input.
  std::uniform_int_distribution<int> noise_bit(-128, 127);

  for (int t = 0; t < kTrials; t++) {
    std::vector<int16_t> llr(encoder_len);
    for (auto& v : llr) {
      v = (int16_t)noise_bit(rng_);
    }
    nr_pdcch_blind_result_t out;
    if (nr_pdcch_blind_decode_and_extract(llr.data(), kAggregationLevel, dci_length, bwp_size,
                                          kDmrsTypeAPositionPos2, NR_PDCCH_BLIND_RNTI_MIN_DEFAULT,
                                          NR_PDCCH_BLIND_RNTI_MAX_DEFAULT, &out)) {
      accepted++;
    }
  }

  const double rate = (double)accepted / kTrials;
  // Estimate (see this project's plan/handover doc): ~0.39% per candidate from the CRC-recovered
  // value landing in the ~65488-value plausible RNTI range out of a ~2^24 CRC space, ASSUMING a
  // garbage decode's crc is uniform over that space. Bound generously (0.03%-5%, roughly half an
  // order of magnitude either side) around that estimate to avoid flakiness while still catching a
  // gross implementation error (filter disabled entirely -> near 100%; filter rejecting everything
  // -> exactly 0, which is itself improbable at the true rate and this trial count).
  EXPECT_GE(accepted, 1) << "0/3000 accepted at an expected ~0.39% rate suggests the plausibility "
                            "filter is rejecting everything, not filtering selectively";
  EXPECT_LT(rate, 0.05) << "measured false-accept rate " << (rate * 100.0)
                        << "% is far above the ~0.39% estimate -- RNTI range check or field bound "
                           "checks may not be engaging";
}


// ---------------------------------------------------------------------------------------------
// Group 5: DCI format 1_0 (TS 38.212 7.3.1.2.1). Added 2026-08-21.
//
// The packers below are written from the SPEC field lists, deliberately NOT by reading
// nr_pdcch_blind_monitor.c's extractor -- a test that mirrors the implementation proves only that
// the code is self-consistent. They were cross-checked field-by-field against this codebase's own
// gNB packer (gNB_scheduler_primitives.c's NR_DL_DCI_FORMAT_1_0 case), which is a genuinely
// independent implementation of the same table and is what actually transmits on this deployment.
// ---------------------------------------------------------------------------------------------

/// Bit-appender shared by the five format-1_0 packers: MSB-first, exactly as the gNB writes from
/// `dci_size` downward.
struct BitPacker {
  uint64_t p = 0;
  int      n = 0;
  void put(uint32_t val, int nbits) {
    if (nbits == 0) return;
    const uint32_t mask = (nbits >= 32) ? 0xFFFFFFFFu : ((1u << nbits) - 1u);
    p = (p << nbits) | (val & mask);
    n += nbits;
  }
};

struct Dci10Gt {
  uint32_t riv         = 0;
  uint32_t tda         = 0;
  uint32_t vrb         = 0;
  uint32_t mcs         = 0;
  uint32_t ndi         = 0;
  uint32_t rv          = 0;
  uint32_t harq_pid    = 0;
  uint32_t dai         = 0;
  uint32_t tpc         = 0;
  uint32_t pucch_ri    = 0;
  uint32_t k1          = 0;
  uint32_t tb_scaling  = 0;
  uint32_t si_ind      = 0;
  uint32_t sm_ind      = 3;
  uint32_t sm          = 0;
  uint32_t reserved    = 0; ///< deliberately settable: the reserved-bit checks are load-bearing
  uint32_t pad_bits    = 0; ///< TS 38.212 7.3.1.0 UE-specific-search-space size alignment
  uint32_t pad_value   = 0;
};

/// C-RNTI / TC-RNTI: identifier(1) freq(riv) tda(4) vrb(1) mcs(5) ndi(1) rv(2) harq(4) dai(2)
/// tpc(2) pucch-ri(3) k1(3) = 28 + riv.
uint64_t PackDci10Crnti(const Dci10Gt& gt, int riv_bits, uint32_t identifier = 1) {
  BitPacker b;
  b.put(identifier, 1);
  b.put(gt.riv, riv_bits);
  b.put(gt.tda, 4);
  b.put(gt.vrb, 1);
  b.put(gt.mcs, 5);
  b.put(gt.ndi, 1);
  b.put(gt.rv, 2);
  b.put(gt.harq_pid, 4);
  b.put(gt.dai, 2);
  b.put(gt.tpc, 2);
  b.put(gt.pucch_ri, 3);
  b.put(gt.k1, 3);
  b.put(gt.pad_value, gt.pad_bits);
  return b.p;
}

/// SI-RNTI: freq(riv) tda(4) vrb(1) mcs(5) rv(2) si-indicator(1) reserved(15) = 28 + riv.
uint64_t PackDci10Si(const Dci10Gt& gt, int riv_bits) {
  BitPacker b;
  b.put(gt.riv, riv_bits);
  b.put(gt.tda, 4);
  b.put(gt.vrb, 1);
  b.put(gt.mcs, 5);
  b.put(gt.rv, 2);
  b.put(gt.si_ind, 1);
  b.put(gt.reserved, 15);
  return b.p;
}

/// RA-RNTI: freq(riv) tda(4) vrb(1) mcs(5) tb-scaling(2) reserved(16) = 28 + riv.
uint64_t PackDci10Ra(const Dci10Gt& gt, int riv_bits) {
  BitPacker b;
  b.put(gt.riv, riv_bits);
  b.put(gt.tda, 4);
  b.put(gt.vrb, 1);
  b.put(gt.mcs, 5);
  b.put(gt.tb_scaling, 2);
  b.put(gt.reserved, 16);
  return b.p;
}

/// P-RNTI: sm-indicator(2) short-message(8) freq(riv) tda(4) vrb(1) mcs(5) tb-scaling(2)
/// reserved(6) = 28 + riv.
uint64_t PackDci10P(const Dci10Gt& gt, int riv_bits) {
  BitPacker b;
  b.put(gt.sm_ind, 2);
  b.put(gt.sm, 8);
  b.put(gt.riv, riv_bits);
  b.put(gt.tda, 4);
  b.put(gt.vrb, 1);
  b.put(gt.mcs, 5);
  b.put(gt.tb_scaling, 2);
  b.put(gt.reserved, 6);
  return b.p;
}

/// The deployment's real pdsch-ConfigCommon list, live-derived from this cell's SIB1 2026-08-21
/// (2 entries: SLIV 40 -> S=1/L=13, SLIV 85 -> S=1/L=7). Used as BOTH lists unless a test
/// deliberately makes them differ.
nr_pdcch_blind_extract_opts_t OptsWithTdaLists(bool separate_common = false) {
  nr_pdcch_blind_extract_opts_t o = DefaultOpts();
  o.tda_count      = 2;
  o.tda_start[0]   = 1;  o.tda_length[0]  = 13; o.tda_mapping[0] = 0;
  o.tda_start[1]   = 1;  o.tda_length[1]  = 7;  o.tda_mapping[1] = 0;
  if (separate_common) {
    // A deliberately DIFFERENT common list, so a test can tell which one was consulted.
    o.tda_common_count     = 2;
    o.tda_common_start[0]  = 2; o.tda_common_length[0] = 4; o.tda_common_mapping[0] = 0;
    o.tda_common_start[1]  = 2; o.tda_common_length[1] = 9; o.tda_common_mapping[1] = 0;
  }
  return o;
}

nr_pdcch_blind_dci10_ctx_t Dci10Ctx(uint8_t ss_type, uint16_t n_rb_riv) {
  nr_pdcch_blind_dci10_ctx_t c = {};
  c.ss_type             = ss_type;
  c.n_rb_riv            = n_rb_riv;
  c.rb_offset           = 0;
  c.dmrs_typeA_position = kDmrsTypeAPositionPos2;
  c.mux_pattern         = 1;
  c.sib1                = 0;
  c.rnti_class_mask     = 0;
  return c;
}

// --- Sizing -------------------------------------------------------------------------------------

TEST(Dci10Size, MatchesTheSpecFormula) {
  // 28 fixed bits + ceil(log2(N(N+1)/2)). At the two bandwidths this project runs:
  EXPECT_EQ(nr_pdcch_blind_dci10_size(106), 28 + 13); // 106*107/2 = 5671 -> 13 bits
  EXPECT_EQ(nr_pdcch_blind_dci10_size(273), 28 + 16); // 273*274/2 = 37401 -> 16 bits
  EXPECT_EQ(nr_pdcch_blind_dci10_size(48), 28 + 11);  // a typical CORESET#0 size
  EXPECT_EQ(nr_pdcch_blind_dci10_size(0), 0);
}

TEST(Dci10Size, AllFiveRntiVariantsAreTheSameWidth) {
  // This is the structural fact the whole design rests on (one decode, several hypotheses), so it
  // is asserted directly rather than assumed: pack each variant and check the bit count.
  const int riv_bits = RivBitsFor(273);
  Dci10Gt gt;
  BitPacker c, si, ra, p;
  c.put(1,1); c.put(0,riv_bits); c.put(0,4); c.put(0,1); c.put(0,5); c.put(0,1); c.put(0,2);
  c.put(0,4); c.put(0,2); c.put(0,2); c.put(0,3); c.put(0,3);
  si.put(0,riv_bits); si.put(0,4); si.put(0,1); si.put(0,5); si.put(0,2); si.put(0,1); si.put(0,15);
  ra.put(0,riv_bits); ra.put(0,4); ra.put(0,1); ra.put(0,5); ra.put(0,2); ra.put(0,16);
  p.put(0,2); p.put(0,8); p.put(0,riv_bits); p.put(0,4); p.put(0,1); p.put(0,5); p.put(0,2); p.put(0,6);
  EXPECT_EQ(c.n, nr_pdcch_blind_dci10_size(273));
  EXPECT_EQ(si.n, c.n);
  EXPECT_EQ(ra.n, c.n);
  EXPECT_EQ(p.n, c.n);
  (void)gt;
}

TEST(Dci00Size, MatchesTheSpecFormula) {
  // 20 fixed bits + RIV (+1 with a supplementary uplink). Exists only for TS 38.212 7.3.1.0's
  // UE-specific-search-space size alignment, so the useful assertion is the COMPARISON: at equal
  // DL/UL bandwidth format 1_0 is the larger of the two, i.e. no padding is applied on this cell.
  EXPECT_EQ(nr_pdcch_blind_dci00_size(273, 0), 20 + 16);
  EXPECT_EQ(nr_pdcch_blind_dci00_size(273, 1), 20 + 16 + 1);
  EXPECT_GT(nr_pdcch_blind_dci10_size(273), nr_pdcch_blind_dci00_size(273, 0));
}

// --- Field extraction, per RNTI class -----------------------------------------------------------

TEST_F(BlindPdcchTest, Dci10CrntiExtractsEveryField) {
  const uint16_t bwp = 273;
  const int      riv_bits = RivBitsFor(bwp);
  const uint16_t len = nr_pdcch_blind_dci10_size(bwp);
  auto opts = OptsWithTdaLists();

  Dci10Gt gt;
  gt.riv      = 275; // start 2, length 3 under NRRIV2BW/NRRIV2PRBOFFSET
  gt.tda      = 1;   // -> S=1 L=7 from the dedicated list
  gt.mcs      = 9;
  gt.ndi      = 1;
  gt.rv       = 2;
  gt.harq_pid = 5;
  const uint64_t payload = PackDci10Crnti(gt, riv_bits);
  auto llr = EncodeToLLR(payload, 0x4601, len, kAggregationLevel, 40.0, rng_);

  auto ctx = Dci10Ctx(NR_BLIND_SS_UE_SPECIFIC, bwp);
  nr_pdcch_blind_result_t out;
  ASSERT_TRUE(nr_pdcch_blind_decode_and_extract_10(llr.data(), kAggregationLevel, len, &ctx, 0x0001, 0xFFEF,
                                                   &opts, &out))
      << (out.reject_reason ? out.reject_reason : "");
  EXPECT_EQ(out.rnti, 0x4601);
  EXPECT_EQ(out.dci_format, NR_BLIND_DCI_FORMAT_1_0);
  EXPECT_EQ(out.rnti_class, NR_BLIND_RNTI_CLASS_C);
  EXPECT_EQ(out.mcs, 9);
  EXPECT_EQ(out.ndi, 1);
  EXPECT_EQ(out.rv, 2);
  EXPECT_EQ(out.harq_pid, 5);
  EXPECT_EQ(out.tda_index, 1);
  EXPECT_EQ(out.start_symbol, 1);
  EXPECT_EQ(out.num_symbols, 7);
  // TS 38.214 5.1.6.2 / 5.1.6.1.3: format 1_0 is single-port (1000), no DM-RS sequence init field,
  // and 2 CDM groups without data for any allocation that is not exactly 2 symbols.
  EXPECT_EQ(out.dmrs_ports, 1);
  EXPECT_EQ(out.nscid, 0);
  EXPECT_EQ(out.n_dmrs_cdm_groups, 2);
  // TS 38.214 5.1.3.1: format 1_0 always indexes Table 5.1.3.1-1.
  EXPECT_EQ(out.mcs_table, 0);
}

TEST_F(BlindPdcchTest, Dci10TwoSymbolAllocationUsesOneCdmGroup) {
  // The OTHER branch of TS 38.214 5.1.6.1.3, and the one that matters downstream: at
  // n_dmrs_cdm_groups == 1 the DM-RS symbol still carries data in the unreserved CDM group, which
  // is the case the data-aided RE enumeration has to handle explicitly.
  const uint16_t bwp = 273;
  const int      riv_bits = RivBitsFor(bwp);
  const uint16_t len = nr_pdcch_blind_dci10_size(bwp);
  auto opts = OptsWithTdaLists();
  opts.tda_count    = 1;
  opts.tda_start[0] = 2;
  opts.tda_length[0] = 2; // exactly 2 symbols
  opts.tda_mapping[0] = 1; // typeB: type A would need S <= dmrs_TypeA_Position

  Dci10Gt gt;
  gt.riv = 275;
  gt.tda = 0;
  const uint64_t payload = PackDci10Crnti(gt, riv_bits);
  auto llr = EncodeToLLR(payload, 0x1234, len, kAggregationLevel, 40.0, rng_);
  auto ctx = Dci10Ctx(NR_BLIND_SS_UE_SPECIFIC, bwp);
  nr_pdcch_blind_result_t out;
  ASSERT_TRUE(nr_pdcch_blind_decode_and_extract_10(llr.data(), kAggregationLevel, len, &ctx, 0x0001, 0xFFEF,
                                                   &opts, &out))
      << (out.reject_reason ? out.reject_reason : "");
  EXPECT_EQ(out.num_symbols, 2);
  EXPECT_EQ(out.n_dmrs_cdm_groups, 1);
}

TEST_F(BlindPdcchTest, Dci10SiRntiIsAcceptedOutsideTheDynamicRange) {
  // SI-RNTI is 0xFFFF, deliberately OUTSIDE the plausible C-RNTI range the caller passes. The
  // format 1_1 path rejects it there; the 1_0 path must admit it on class, not on range.
  const uint16_t cset0 = 48;
  const int      riv_bits = RivBitsFor(cset0);
  const uint16_t len = nr_pdcch_blind_dci10_size(cset0);
  auto opts = OptsWithTdaLists();

  Dci10Gt gt;
  gt.riv    = 100;
  gt.tda    = 0;
  gt.mcs    = 4;
  gt.rv     = 1;
  gt.si_ind = 1;
  const uint64_t payload = PackDci10Si(gt, riv_bits);
  auto llr = EncodeToLLR(payload, 0xFFFF, len, kAggregationLevel, 40.0, rng_);

  auto ctx = Dci10Ctx(NR_BLIND_SS_COMMON, cset0);
  nr_pdcch_blind_result_t out;
  ASSERT_TRUE(nr_pdcch_blind_decode_and_extract_10(llr.data(), kAggregationLevel, len, &ctx, 0x0001, 0xFFEF,
                                                   &opts, &out))
      << (out.reject_reason ? out.reject_reason : "");
  EXPECT_EQ(out.rnti, 0xFFFF);
  EXPECT_EQ(out.rnti_class, NR_BLIND_RNTI_CLASS_SI);
  EXPECT_EQ(out.si_indicator, 1);
  EXPECT_EQ(out.mcs, 4);
  EXPECT_EQ(out.rv, 1);
}

TEST_F(BlindPdcchTest, Dci10SiRntiRejectsNonZeroReservedBits) {
  // The 15 spec-fixed zero bits are this format's strongest false-accept discriminator. If they are
  // not actually checked, a random payload that CRC-matches 0xFFFF becomes a confident SIB1 grant.
  const uint16_t cset0 = 48;
  const int      riv_bits = RivBitsFor(cset0);
  const uint16_t len = nr_pdcch_blind_dci10_size(cset0);
  auto opts = OptsWithTdaLists();

  Dci10Gt gt;
  gt.riv      = 100;
  gt.reserved = 1; // a single bit is enough
  const uint64_t payload = PackDci10Si(gt, riv_bits);
  auto llr = EncodeToLLR(payload, 0xFFFF, len, kAggregationLevel, 40.0, rng_);

  auto ctx = Dci10Ctx(NR_BLIND_SS_COMMON, cset0);
  nr_pdcch_blind_result_t out;
  EXPECT_FALSE(nr_pdcch_blind_decode_and_extract_10(llr.data(), kAggregationLevel, len, &ctx, 0x0001, 0xFFEF,
                                                    &opts, &out));
  EXPECT_EQ(out.rnti, 0xFFFF); // still reported, so the caller can see WHAT was rejected
  ASSERT_NE(out.reject_reason, nullptr);
  EXPECT_NE(std::string(out.reject_reason).find("reserved"), std::string::npos);
}

TEST_F(BlindPdcchTest, Dci10RaRntiCarriesTbScaling) {
  const uint16_t cset0 = 48;
  const int      riv_bits = RivBitsFor(cset0);
  const uint16_t len = nr_pdcch_blind_dci10_size(cset0);
  auto opts = OptsWithTdaLists();

  Dci10Gt gt;
  gt.riv        = 100;
  gt.tda        = 1;
  gt.mcs        = 3;
  gt.tb_scaling = 1; // S = 0.5
  const uint64_t payload = PackDci10Ra(gt, riv_bits);
  auto llr = EncodeToLLR(payload, 0x0011, len, kAggregationLevel, 40.0, rng_); // a valid RA-RNTI

  auto ctx = Dci10Ctx(NR_BLIND_SS_COMMON, cset0);
  nr_pdcch_blind_result_t out;
  ASSERT_TRUE(nr_pdcch_blind_decode_and_extract_10(llr.data(), kAggregationLevel, len, &ctx, 0x0001, 0xFFEF,
                                                   &opts, &out))
      << (out.reject_reason ? out.reject_reason : "");
  EXPECT_EQ(out.rnti_class, NR_BLIND_RNTI_CLASS_RA);
  EXPECT_EQ(out.tb_scaling, 1);
  EXPECT_EQ(out.mcs, 3);
}

TEST_F(BlindPdcchTest, Dci10RaRntiRejectsTheReservedTbScalingCodePoint) {
  // TS 38.214 Table 5.1.3.2-2 defines three scaling factors; the fourth code point is reserved.
  const uint16_t cset0 = 48;
  const int      riv_bits = RivBitsFor(cset0);
  const uint16_t len = nr_pdcch_blind_dci10_size(cset0);
  auto opts = OptsWithTdaLists();

  Dci10Gt gt;
  gt.riv        = 100;
  gt.tb_scaling = 3;
  const uint64_t payload = PackDci10Ra(gt, riv_bits);
  auto llr = EncodeToLLR(payload, 0x0011, len, kAggregationLevel, 40.0, rng_);

  auto ctx = Dci10Ctx(NR_BLIND_SS_COMMON, cset0);
  ctx.rnti_class_mask = 1u << NR_BLIND_RNTI_CLASS_RA; // isolate the class under test
  nr_pdcch_blind_result_t out;
  EXPECT_FALSE(nr_pdcch_blind_decode_and_extract_10(llr.data(), kAggregationLevel, len, &ctx, 0x0001, 0xFFEF,
                                                    &opts, &out));
  ASSERT_NE(out.reject_reason, nullptr);
  EXPECT_NE(std::string(out.reject_reason).find("TB scaling"), std::string::npos);
}

TEST_F(BlindPdcchTest, Dci10RaRntiIsNotAttemptedAboveTheSpecMaximum) {
  // TS 38.321 5.1.3 bounds RA-RNTI at 17920. Above that the value CANNOT be an RA-RNTI, which is
  // the one spec-derived way to stop an RA hypothesis from shadowing a TC/C-RNTI grant.
  const uint16_t cset0 = 48;
  const int      riv_bits = RivBitsFor(cset0);
  const uint16_t len = nr_pdcch_blind_dci10_size(cset0);
  auto opts = OptsWithTdaLists();

  // A payload that would parse cleanly AS an RA grant (16 zero reserved bits, valid scaling).
  Dci10Gt gt;
  gt.riv = 100;
  const uint64_t payload = PackDci10Ra(gt, riv_bits);
  auto ctx = Dci10Ctx(NR_BLIND_SS_COMMON, cset0);
  ctx.rnti_class_mask = 1u << NR_BLIND_RNTI_CLASS_RA;

  nr_pdcch_blind_result_t out;
  auto in_range = EncodeToLLR(payload, NR_PDCCH_BLIND_RA_RNTI_MAX, len, kAggregationLevel, 40.0, rng_);
  EXPECT_TRUE(nr_pdcch_blind_decode_and_extract_10(in_range.data(), kAggregationLevel, len, &ctx, 0x0001,
                                                   0xFFEF, &opts, &out))
      << (out.reject_reason ? out.reject_reason : "");
  auto out_of_range = EncodeToLLR(payload, NR_PDCCH_BLIND_RA_RNTI_MAX + 1, len, kAggregationLevel, 40.0, rng_);
  EXPECT_FALSE(nr_pdcch_blind_decode_and_extract_10(out_of_range.data(), kAggregationLevel, len, &ctx, 0x0001,
                                                    0xFFEF, &opts, &out));
}

TEST_F(BlindPdcchTest, Dci10PRntiShortMessageOnlyCarriesNoGrant) {
  // TS 38.331 shortMessageIndicator: 10 = short message only. There is no PDSCH to point at, so
  // reporting an allocation from those bits would be inventing one.
  const uint16_t cset0 = 48;
  const int      riv_bits = RivBitsFor(cset0);
  const uint16_t len = nr_pdcch_blind_dci10_size(cset0);
  auto opts = OptsWithTdaLists();
  auto ctx  = Dci10Ctx(NR_BLIND_SS_COMMON, cset0);

  Dci10Gt gt;
  gt.riv = 100;
  gt.sm  = 0xA5;

  nr_pdcch_blind_result_t out;
  gt.sm_ind = 3; // paging + short message: a real grant
  auto both = EncodeToLLR(PackDci10P(gt, riv_bits), 0xFFFE, len, kAggregationLevel, 40.0, rng_);
  ASSERT_TRUE(nr_pdcch_blind_decode_and_extract_10(both.data(), kAggregationLevel, len, &ctx, 0x0001, 0xFFEF,
                                                   &opts, &out))
      << (out.reject_reason ? out.reject_reason : "");
  EXPECT_EQ(out.rnti_class, NR_BLIND_RNTI_CLASS_P);
  EXPECT_EQ(out.short_messages, 0xA5);
  EXPECT_EQ(out.short_messages_ind, 3);

  gt.sm_ind = 2; // short message only
  auto sm_only = EncodeToLLR(PackDci10P(gt, riv_bits), 0xFFFE, len, kAggregationLevel, 40.0, rng_);
  EXPECT_FALSE(nr_pdcch_blind_decode_and_extract_10(sm_only.data(), kAggregationLevel, len, &ctx, 0x0001,
                                                    0xFFEF, &opts, &out));
}

// --- The three things the search-space kind changes ----------------------------------------------

TEST_F(BlindPdcchTest, Dci10CommonSearchSpaceUsesTheCommonTdaList) {
  // TS 38.214 Table 5.1.2.1.1-1: the DEDICATED pdsch-Config list applies only to C-RNTI in a
  // UE-specific search space. Everything else takes pdsch-ConfigCommon's. Made observable by giving
  // the two lists different symbol allocations.
  const uint16_t cset0 = 48;
  const int      riv_bits = RivBitsFor(cset0);
  const uint16_t len = nr_pdcch_blind_dci10_size(cset0);
  auto opts = OptsWithTdaLists(/*separate_common=*/true);

  Dci10Gt gt;
  gt.riv = 100;
  gt.tda = 0;
  nr_pdcch_blind_result_t out;

  // Common search space, TC-RNTI -> the COMMON list (S=2, L=4).
  auto ctx_css = Dci10Ctx(NR_BLIND_SS_COMMON, cset0);
  ctx_css.rnti_class_mask = 1u << NR_BLIND_RNTI_CLASS_TC;
  auto llr_css = EncodeToLLR(PackDci10Crnti(gt, riv_bits), 0x4601, len, kAggregationLevel, 40.0, rng_);
  ASSERT_TRUE(nr_pdcch_blind_decode_and_extract_10(llr_css.data(), kAggregationLevel, len, &ctx_css, 0x0001,
                                                   0xFFEF, &opts, &out))
      << (out.reject_reason ? out.reject_reason : "");
  EXPECT_EQ(out.rnti_class, NR_BLIND_RNTI_CLASS_TC);
  EXPECT_EQ(out.start_symbol, 2);
  EXPECT_EQ(out.num_symbols, 4);

  // UE-specific search space, C-RNTI, SAME payload -> the DEDICATED list (S=1, L=13).
  const uint16_t bwp = 273;
  const int      riv_bits_uss = RivBitsFor(bwp);
  const uint16_t len_uss = nr_pdcch_blind_dci10_size(bwp);
  auto llr_uss = EncodeToLLR(PackDci10Crnti(gt, riv_bits_uss), 0x4601, len_uss, kAggregationLevel, 40.0, rng_);
  auto ctx_uss = Dci10Ctx(NR_BLIND_SS_UE_SPECIFIC, bwp);
  ASSERT_TRUE(nr_pdcch_blind_decode_and_extract_10(llr_uss.data(), kAggregationLevel, len_uss, &ctx_uss,
                                                   0x0001, 0xFFEF, &opts, &out))
      << (out.reject_reason ? out.reject_reason : "");
  EXPECT_EQ(out.rnti_class, NR_BLIND_RNTI_CLASS_C);
  EXPECT_EQ(out.start_symbol, 1);
  EXPECT_EQ(out.num_symbols, 13);
}

TEST_F(BlindPdcchTest, Dci10FrequencyReferenceComesFromTheContextNotTheBwp) {
  // TS 38.212 7.3.1.0 sizes the frequency-domain field from CORESET#0 in a common search space. The
  // SAME RIV value therefore names a DIFFERENT allocation depending on n_rb_riv -- so this asserts
  // that the context, not a BWP constant, drives the decode.
  auto opts = OptsWithTdaLists();
  Dci10Gt gt;
  gt.tda = 0;
  nr_pdcch_blind_result_t out;

  // RIV for start=3, length=2 in a 48-RB reference: (L-1)*N + S = 1*48 + 3 = 51.
  gt.riv = 51;
  const uint16_t len48 = nr_pdcch_blind_dci10_size(48);
  auto ctx48 = Dci10Ctx(NR_BLIND_SS_COMMON, 48);
  ctx48.rnti_class_mask = 1u << NR_BLIND_RNTI_CLASS_TC;
  auto llr48 = EncodeToLLR(PackDci10Crnti(gt, RivBitsFor(48)), 0x4601, len48, kAggregationLevel, 40.0, rng_);
  ASSERT_TRUE(nr_pdcch_blind_decode_and_extract_10(llr48.data(), kAggregationLevel, len48, &ctx48, 0x0001,
                                                   0xFFEF, &opts, &out))
      << (out.reject_reason ? out.reject_reason : "");
  EXPECT_EQ(out.start_rb, 3);
  EXPECT_EQ(out.num_rb, 2);

  // Same RIV against a 273-RB reference is start=51, length=1 -- a different allocation entirely.
  const uint16_t len273 = nr_pdcch_blind_dci10_size(273);
  auto ctx273 = Dci10Ctx(NR_BLIND_SS_UE_SPECIFIC, 273);
  auto llr273 = EncodeToLLR(PackDci10Crnti(gt, RivBitsFor(273)), 0x4601, len273, kAggregationLevel, 40.0, rng_);
  ASSERT_TRUE(nr_pdcch_blind_decode_and_extract_10(llr273.data(), kAggregationLevel, len273, &ctx273, 0x0001,
                                                   0xFFEF, &opts, &out))
      << (out.reject_reason ? out.reject_reason : "");
  EXPECT_EQ(out.start_rb, 51);
  EXPECT_EQ(out.num_rb, 1);
}

TEST_F(BlindPdcchTest, Dci10SibOneFallsBackToTheDefaultTdaTable) {
  // pdsch-ConfigCommon travels INSIDE SIB1, so a receiver decoding SIB1 cannot have its TDRA list
  // yet: TS 38.214 Table 5.1.2.1.1-1 sends it to the default table selected by the SS/PBCH-to-
  // CORESET#0 multiplexing pattern. Asserted by checking the configured list is NOT used.
  const uint16_t cset0 = 48;
  const int      riv_bits = RivBitsFor(cset0);
  const uint16_t len = nr_pdcch_blind_dci10_size(cset0);
  auto opts = OptsWithTdaLists(/*separate_common=*/true); // common list is S=2/L=4 at index 0

  Dci10Gt gt;
  gt.riv = 100;
  gt.tda = 0;
  auto llr = EncodeToLLR(PackDci10Si(gt, riv_bits), 0xFFFF, len, kAggregationLevel, 40.0, rng_);

  auto ctx = Dci10Ctx(NR_BLIND_SS_COMMON, cset0);
  ctx.sib1        = 1;
  ctx.mux_pattern = 1; // -> default table A
  nr_pdcch_blind_result_t out;
  ASSERT_TRUE(nr_pdcch_blind_decode_and_extract_10(llr.data(), kAggregationLevel, len, &ctx, 0x0001, 0xFFEF,
                                                   &opts, &out))
      << (out.reject_reason ? out.reject_reason : "");
  // Default table A row 0 at dmrs-TypeA-Position pos2 is S=2, L=12 -- and crucially NOT the
  // configured common list's S=2/L=4.
  EXPECT_EQ(out.num_symbols, 12);
  EXPECT_NE(out.num_symbols, 4);
}

// --- Rejections that keep a blind scan honest -----------------------------------------------------

TEST_F(BlindPdcchTest, Dci10RejectsTheUplinkFormatZeroZero) {
  // Formats 0_0 and 1_0 share a payload size by construction (TS 38.212 7.3.1.0), so the identifier
  // bit is the ONLY thing separating an UL grant from a DL one.
  const uint16_t bwp = 273;
  const uint16_t len = nr_pdcch_blind_dci10_size(bwp);
  auto opts = OptsWithTdaLists();
  Dci10Gt gt;
  gt.riv = 275;
  auto llr = EncodeToLLR(PackDci10Crnti(gt, RivBitsFor(bwp), /*identifier=*/0), 0x4601, len, kAggregationLevel,
                         40.0, rng_);
  auto ctx = Dci10Ctx(NR_BLIND_SS_UE_SPECIFIC, bwp);
  nr_pdcch_blind_result_t out;
  EXPECT_FALSE(nr_pdcch_blind_decode_and_extract_10(llr.data(), kAggregationLevel, len, &ctx, 0x0001, 0xFFEF,
                                                    &opts, &out));
  ASSERT_NE(out.reject_reason, nullptr);
  EXPECT_NE(std::string(out.reject_reason).find("0_0"), std::string::npos);
}

TEST_F(BlindPdcchTest, Dci10RejectsAPdcchOrder) {
  // TS 38.212 7.3.1.2.1: an all-ones frequency-domain field is a PDCCH order (start random access),
  // and the bits after it are preamble/SSB/PRACH-mask, not a grant. Reading them as one produces a
  // confident, wrong allocation -- which is exactly the failure class this module keeps hitting.
  const uint16_t bwp = 273;
  const int      riv_bits = RivBitsFor(bwp);
  const uint16_t len = nr_pdcch_blind_dci10_size(bwp);
  auto opts = OptsWithTdaLists();
  Dci10Gt gt;
  gt.riv = (1u << riv_bits) - 1u;
  auto llr = EncodeToLLR(PackDci10Crnti(gt, riv_bits), 0x4601, len, kAggregationLevel, 40.0, rng_);
  auto ctx = Dci10Ctx(NR_BLIND_SS_UE_SPECIFIC, bwp);
  nr_pdcch_blind_result_t out;
  EXPECT_FALSE(nr_pdcch_blind_decode_and_extract_10(llr.data(), kAggregationLevel, len, &ctx, 0x0001, 0xFFEF,
                                                    &opts, &out));
  ASSERT_NE(out.reject_reason, nullptr);
  EXPECT_NE(std::string(out.reject_reason).find("PDCCH order"), std::string::npos);
}

TEST_F(BlindPdcchTest, Dci10RejectsInterleavedVrbToPrbRatherThanMisreadingIt) {
  // Interleaved VRB-to-PRB mapping (TS 38.211 7.3.1.6) permutes the allocation in 2-RB bundles, so
  // the PRBs are NOT the contiguous set the RIV names. OAI's UE PHY has no de-interleaver, so the
  // only honest outcome is a labelled rejection -- silently extracting the wrong REs would look
  // like a weak channel instead of a wrong one.
  const uint16_t bwp = 273;
  const uint16_t len = nr_pdcch_blind_dci10_size(bwp);
  auto opts = OptsWithTdaLists();
  Dci10Gt gt;
  gt.riv = 275;
  gt.vrb = 1;
  auto llr = EncodeToLLR(PackDci10Crnti(gt, RivBitsFor(bwp)), 0x4601, len, kAggregationLevel, 40.0, rng_);
  auto ctx = Dci10Ctx(NR_BLIND_SS_UE_SPECIFIC, bwp);
  nr_pdcch_blind_result_t out;
  EXPECT_FALSE(nr_pdcch_blind_decode_and_extract_10(llr.data(), kAggregationLevel, len, &ctx, 0x0001, 0xFFEF,
                                                    &opts, &out));
  ASSERT_NE(out.reject_reason, nullptr);
  EXPECT_NE(std::string(out.reject_reason).find("interleaved"), std::string::npos);
}

TEST_F(BlindPdcchTest, Dci10RejectsATdaIndexBeyondTheList) {
  // The time-domain field is ALWAYS 4 bits in format 1_0 (unlike 1_1's ceil(log2(count))), so on a
  // 2-entry list 14 of the 16 code points are impossible -- a genuinely useful false-accept filter.
  const uint16_t bwp = 273;
  const uint16_t len = nr_pdcch_blind_dci10_size(bwp);
  auto opts = OptsWithTdaLists();
  Dci10Gt gt;
  gt.riv = 275;
  gt.tda = 7;
  auto llr = EncodeToLLR(PackDci10Crnti(gt, RivBitsFor(bwp)), 0x4601, len, kAggregationLevel, 40.0, rng_);
  auto ctx = Dci10Ctx(NR_BLIND_SS_UE_SPECIFIC, bwp);
  nr_pdcch_blind_result_t out;
  EXPECT_FALSE(nr_pdcch_blind_decode_and_extract_10(llr.data(), kAggregationLevel, len, &ctx, 0x0001, 0xFFEF,
                                                    &opts, &out));
  ASSERT_NE(out.reject_reason, nullptr);
  EXPECT_NE(std::string(out.reject_reason).find("TDRA"), std::string::npos);
}

TEST_F(BlindPdcchTest, Dci10RejectsTheReservedMcsRange) {
  // Format 1_0 always indexes Table 5.1.3.1-1, where 0..28 are valid and 29..31 are reserved. Note
  // this bound differs from the format 1_1 path's >=28, which is table 2's -- see the comment at
  // that check for why the two are deliberately different rather than inconsistent.
  const uint16_t bwp = 273;
  const uint16_t len = nr_pdcch_blind_dci10_size(bwp);
  auto opts = OptsWithTdaLists();
  auto ctx  = Dci10Ctx(NR_BLIND_SS_UE_SPECIFIC, bwp);
  nr_pdcch_blind_result_t out;

  Dci10Gt gt;
  gt.riv = 275;
  gt.mcs = 28; // the last VALID entry of table 1
  auto ok = EncodeToLLR(PackDci10Crnti(gt, RivBitsFor(bwp)), 0x4601, len, kAggregationLevel, 40.0, rng_);
  EXPECT_TRUE(nr_pdcch_blind_decode_and_extract_10(ok.data(), kAggregationLevel, len, &ctx, 0x0001, 0xFFEF,
                                                   &opts, &out))
      << (out.reject_reason ? out.reject_reason : "");
  gt.mcs = 29; // reserved
  auto bad = EncodeToLLR(PackDci10Crnti(gt, RivBitsFor(bwp)), 0x4601, len, kAggregationLevel, 40.0, rng_);
  EXPECT_FALSE(nr_pdcch_blind_decode_and_extract_10(bad.data(), kAggregationLevel, len, &ctx, 0x0001, 0xFFEF,
                                                    &opts, &out));
}

TEST_F(BlindPdcchTest, Dci10SizeAlignmentPaddingIsCheckedForZero) {
  // TS 38.212 7.3.1.0 pads the smaller of 0_0/1_0 with ZEROS in a UE-specific search space. Padding
  // that is not zero is not a format 1_0 payload.
  const uint16_t bwp = 273;
  const int      riv_bits = RivBitsFor(bwp);
  const uint16_t base_len = nr_pdcch_blind_dci10_size(bwp);
  const uint16_t len      = base_len + 3;
  auto opts = OptsWithTdaLists();
  auto ctx  = Dci10Ctx(NR_BLIND_SS_UE_SPECIFIC, bwp);
  nr_pdcch_blind_result_t out;

  Dci10Gt gt;
  gt.riv      = 275;
  gt.pad_bits = 3;
  gt.pad_value = 0;
  auto ok = EncodeToLLR(PackDci10Crnti(gt, riv_bits), 0x4601, len, kAggregationLevel, 40.0, rng_);
  EXPECT_TRUE(nr_pdcch_blind_decode_and_extract_10(ok.data(), kAggregationLevel, len, &ctx, 0x0001, 0xFFEF,
                                                   &opts, &out))
      << (out.reject_reason ? out.reject_reason : "");
  gt.pad_value = 5;
  auto bad = EncodeToLLR(PackDci10Crnti(gt, riv_bits), 0x4601, len, kAggregationLevel, 40.0, rng_);
  EXPECT_FALSE(nr_pdcch_blind_decode_and_extract_10(bad.data(), kAggregationLevel, len, &ctx, 0x0001, 0xFFEF,
                                                    &opts, &out));
}

TEST_F(BlindPdcchTest, Dci10ClassMaskNarrowsWhatIsAttempted) {
  // The mask is the cheapest false-accept control on this format: a receiver that only wants RRCSetup
  // should not be inventing SI or paging grants out of noise.
  const uint16_t cset0 = 48;
  const uint16_t len = nr_pdcch_blind_dci10_size(cset0);
  auto opts = OptsWithTdaLists();
  Dci10Gt gt;
  gt.riv = 100;
  auto llr = EncodeToLLR(PackDci10Si(gt, RivBitsFor(cset0)), 0xFFFF, len, kAggregationLevel, 40.0, rng_);
  nr_pdcch_blind_result_t out;

  auto ctx_on = Dci10Ctx(NR_BLIND_SS_COMMON, cset0);
  EXPECT_TRUE(nr_pdcch_blind_decode_and_extract_10(llr.data(), kAggregationLevel, len, &ctx_on, 0x0001, 0xFFEF,
                                                   &opts, &out));
  auto ctx_off = Dci10Ctx(NR_BLIND_SS_COMMON, cset0);
  ctx_off.rnti_class_mask = 1u << NR_BLIND_RNTI_CLASS_TC; // SI not enabled
  EXPECT_FALSE(nr_pdcch_blind_decode_and_extract_10(llr.data(), kAggregationLevel, len, &ctx_off, 0x0001,
                                                    0xFFEF, &opts, &out));
}

TEST_F(BlindPdcchTest, Dci10EitherDynamicClassBitEnablesTheSameHypothesis) {
  // C-RNTI and TC-RNTI are bit-identical on the air and the label is chosen by search space, so a
  // caller asking for "C" in a common search space must not silently get nothing back.
  const uint16_t cset0 = 48;
  const uint16_t len = nr_pdcch_blind_dci10_size(cset0);
  auto opts = OptsWithTdaLists();
  Dci10Gt gt;
  gt.riv = 100;
  gt.tda = 0;
  auto llr = EncodeToLLR(PackDci10Crnti(gt, RivBitsFor(cset0)), 0x4601, len, kAggregationLevel, 40.0, rng_);
  nr_pdcch_blind_result_t out;
  for (uint32_t bit : {(uint32_t)NR_BLIND_RNTI_CLASS_C, (uint32_t)NR_BLIND_RNTI_CLASS_TC}) {
    auto ctx = Dci10Ctx(NR_BLIND_SS_COMMON, cset0);
    ctx.rnti_class_mask = 1u << bit;
    EXPECT_TRUE(nr_pdcch_blind_decode_and_extract_10(llr.data(), kAggregationLevel, len, &ctx, 0x0001, 0xFFEF,
                                                     &opts, &out))
        << "class bit " << bit << ": " << (out.reject_reason ? out.reject_reason : "");
    EXPECT_EQ(out.rnti_class, NR_BLIND_RNTI_CLASS_TC) << "label follows the search space, not the mask";
  }
}

TEST_F(BlindPdcchTest, Dci10PureNoiseFalseAcceptRateIsBounded) {
  // The 1_1 path has this test; 1_0 needs its own because it accepts MORE RNTI values (the two
  // broadcast ones) and tries several hypotheses per decode -- both of which could plausibly raise
  // the false-accept rate, and neither of which is allowed to.
  const uint16_t bwp = 273;
  const uint16_t len = nr_pdcch_blind_dci10_size(bwp);
  auto opts = OptsWithTdaLists();
  auto ctx  = Dci10Ctx(NR_BLIND_SS_UE_SPECIFIC, bwp);

  std::uniform_int_distribution<int> noise(-128, 127);
  int accepts = 0;
  const int kTrials = 2000;
  for (int t = 0; t < kTrials; t++) {
    std::vector<int16_t> llr(kAggregationLevel * 108);
    for (auto& v : llr) v = (int16_t)noise(rng_);
    nr_pdcch_blind_result_t out;
    if (nr_pdcch_blind_decode_and_extract_10(llr.data(), kAggregationLevel, len, &ctx, 0x0001, 0xFFEF, &opts,
                                             &out)) {
      accepts++;
    }
  }
  // A CRC-plausible false decode still has to pass the identifier bit, the MCS/TDRA/RIV/VRB checks
  // and the padding check. The 1_1 test bounds itself the same way; the point is that the wider RNTI
  // admission has not made this materially worse.
  EXPECT_LT(accepts, kTrials / 20) << "accepts=" << accepts << " of " << kTrials;
}

// =============================================================================================
// Group 4: UPLINK DCI formats 0_1 and 0_0.
//
// A note on what these tests can and cannot establish. The DL 1_1 tests can assert a real field
// layout because the deployment's DL widths were pinned field-by-field against the gNB's own log.
// The UL 0_1 layout has NOT been pinned that way yet -- the gNB does not dump its UL RRC config,
// and the 43-bit live payload width admits more than one width assignment (see the header's UL
// section). So these tests assert:
//   - that the packer, the size function and the extractor agree with EACH OTHER for any width
//     assignment (which is what stops the module drifting internally, the failure that produced
//     the DL path's year of wrong offsets), and
//   - the pieces that ARE independently pinned against the live gNB: the DM-RS symbol mask and the
//     antenna-ports code point, both reconciled below against a dumped FAPI PUSCH PDU.
// They deliberately do NOT assert "43 bits is field list X" as truth. That comes from replaying a
// capture against the gNB log, not from a unit test.
// =============================================================================================

/// Resolve one overridable UL width exactly as nr_pdcch_blind_monitor.c's blind_ul_field_bits()
/// does, so a packed test payload always matches the layout the extractor reads.
int UlTdaBits(const nr_pdcch_blind_ul_opts_t& o)
{
  if (o.tda_count <= 0) return 4;
  int b = 0;
  while ((1 << b) < o.tda_count) b++;
  return b;
}

/// One synthetic DCI-0_1 payload's ground truth.
struct UlGroundTruth {
  uint16_t rnti      = 0x4630;
  uint32_t riv       = 0;
  uint32_t tda_index = 0;
  uint32_t mcs       = 10;
  uint32_t ndi       = 1;
  uint32_t rv        = 0;
  uint32_t harq_pid  = 1;
  uint32_t tpc       = 1;
  uint32_t dai       = 2;
  uint32_t antenna_ports = 2;
  uint32_t srs_request   = 0;
  uint32_t csi_request   = 0;
  uint32_t freq_hopping  = 0;
  uint32_t dmrs_seq_init = 0;
  uint32_t ulsch_ind     = 1;
  uint32_t format_ind    = 0; // 0 = UL
};

/// Packs a UlGroundTruth in fill_dci_pdu_rel15()'s NR_UL_DCI_FORMAT_0_1 PACKER order -- the same
/// order nr_pdcch_blind_decode_and_extract_01() walks. If these two ever disagree, every test below
/// fails loudly, which is the point.
uint64_t PackUlPayload(const UlGroundTruth& gt, const nr_pdcch_blind_ul_opts_t& o)
{
  uint64_t p = 0;
  auto put = [&](uint32_t val, int nbits) {
    if (nbits == 0) return;
    const uint32_t mask = (nbits >= 32) ? 0xFFFFFFFFu : ((1u << nbits) - 1u);
    p = (p << nbits) | (val & mask);
  };
  const int riv_bits = RivBitsFor(o.bwp_size);
  put(gt.format_ind, 1);                                   // format identifier (0 = UL)
  put(0, PickBits(o.carrier_indicator_bits, 0));           // carrier indicator
  put(0, PickBits(o.ul_sul_bits, 0));                      // UL/SUL indicator
  put(0, PickBits(o.bwp_indicator_bits, 0));               // BWP indicator
  put(gt.riv, riv_bits);                                   // frequency domain assignment
  put(gt.tda_index, UlTdaBits(o));                         // time domain assignment
  put(gt.freq_hopping, PickBits(o.freq_hopping_bits, 0));  // frequency hopping flag
  put(gt.mcs, 5);                                          // MCS
  put(gt.ndi, 1);                                          // NDI
  put(gt.rv, 2);                                           // RV
  put(gt.harq_pid, PickBits(o.harq_pid_bits, 4));          // HARQ process number
  put(gt.dai, PickBits(o.dai1_bits, 2));                   // 1st DAI
  put(0, PickBits(o.dai2_bits, 0));                        // 2nd DAI
  put(gt.tpc, 2);                                          // TPC for scheduled PUSCH
  put(0, PickBits(o.sri_bits, 0));                         // SRS resource indicator
  put(0, PickBits(o.precoding_info_bits, 0));              // precoding information / layers
  put(gt.antenna_ports, PickBits(o.antenna_ports_bits, 2)); // antenna ports
  put(gt.srs_request, PickBits(o.srs_request_bits, 2));    // SRS request
  put(gt.csi_request, PickBits(o.csi_request_bits, 0));    // CSI request
  put(0, PickBits(o.cbg_bits, 0));                         // CBGTI
  put(0, PickBits(o.ptrs_dmrs_bits, 0));                   // PTRS-DMRS association
  put(0, PickBits(o.beta_offset_bits, 0));                 // beta offset indicator
  put(gt.dmrs_seq_init, PickBits(o.dmrs_seq_init_bits, 1)); // DM-RS sequence initialisation
  put(gt.ulsch_ind, 1);                                    // UL-SCH indicator
  return p;
}

/// UL opts matching THIS deployment as far as it is known (live gNB, 2026-08-25): 273-PRB UL BWP
/// at CRB 0, CP-OFDM, qam64, PCI 2, and the 2-entry TDRA list the PUSCH dump implies
/// (symb=[0..14), k2=4). Widths are left at their documented defaults -- see the group comment for
/// why no width here is claimed to be the pinned truth.
nr_pdcch_blind_ul_opts_t LiveUlOpts()
{
  nr_pdcch_blind_ul_opts_t o = {};
  o.bwp_start = 0;
  o.bwp_size  = 273;
  o.tda_count = 2;
  o.tda_start[0] = 0;  o.tda_length[0] = 14; o.tda_mapping[0] = 0; o.tda_k2[0] = 4;
  o.tda_start[1] = 0;  o.tda_length[1] = 12; o.tda_mapping[1] = 0; o.tda_k2[1] = 4;
  o.dmrs_config_type      = 0;
  o.dmrs_add_pos          = 2;   // reconciled below against the live ul_dmrs_symb_pos dump
  o.dmrs_max_length       = 1;
  o.transform_precoding   = 0;   // live dump says "Transform Precoding Disabled"
  o.mcs_table             = 0;   // live dump says mcs_table=qam64
  o.data_scrambling_id    = -1;
  o.ul_dmrs_scrambling_id = -1;
  o.phy_cell_id           = 2;
  o.carrier_indicator_bits = -1;
  o.ul_sul_bits            = -1;
  o.bwp_indicator_bits     = -1;
  o.freq_hopping_bits      = -1;
  o.harq_pid_bits          = -1;
  o.dai1_bits              = -1;
  o.dai2_bits              = -1;
  o.sri_bits               = -1;
  o.precoding_info_bits    = -1;
  o.antenna_ports_bits     = -1;
  o.srs_request_bits       = -1;
  o.csi_request_bits       = -1;
  o.cbg_bits               = -1;
  o.ptrs_dmrs_bits         = -1;
  o.beta_offset_bits       = -1;
  o.dmrs_seq_init_bits     = -1;
  return o;
}

TEST_F(BlindPdcchTest, Dci01SizeIsTheSumOfItsFieldWidths) {
  nr_pdcch_blind_ul_opts_t o = LiveUlOpts();
  // 12 spec-fixed (fmt 1 + MCS 5 + NDI 1 + RV 2 + TPC 2 + ULSCH-ind 1) + RIV 16 + TDA 1
  // + HARQ 4 + DAI1 2 + antenna ports 2 + SRS request 2 + DM-RS seq init 1 = 40 at every other
  // default. Asserting the arithmetic keeps the size function and the documented defaults honest.
  EXPECT_EQ(nr_pdcch_blind_dci01_size(&o), 40);
  EXPECT_EQ(RivBitsFor(273), 16);

  // Widening any one field moves the total by exactly that much -- i.e. no field is being
  // double-counted or dropped.
  o.freq_hopping_bits = 1;
  EXPECT_EQ(nr_pdcch_blind_dci01_size(&o), 41);
  o.csi_request_bits = 2;
  EXPECT_EQ(nr_pdcch_blind_dci01_size(&o), 43);
}

TEST_F(BlindPdcchTest, Dci01SizeIsZeroForAnInvalidBwp) {
  nr_pdcch_blind_ul_opts_t o = LiveUlOpts();
  o.bwp_size = 0;
  EXPECT_EQ(nr_pdcch_blind_dci01_size(&o), 0);
  EXPECT_EQ(nr_pdcch_blind_dci01_size(nullptr), 0);
}

TEST_F(BlindPdcchTest, Dci01RoundTripRecoversEveryField) {
  nr_pdcch_blind_ul_opts_t o = LiveUlOpts();
  const uint16_t len = nr_pdcch_blind_dci01_size(&o);

  UlGroundTruth gt;
  gt.riv       = 1200;
  gt.tda_index = 0;
  gt.mcs       = 25;
  gt.ndi       = 1;
  gt.rv        = 0;
  gt.harq_pid  = 1;
  gt.tpc       = 1;
  gt.dai       = 2;
  gt.antenna_ports = 2;

  const uint64_t payload = PackUlPayload(gt, o);
  auto llr = EncodeToLLR(payload, gt.rnti, len, kAggregationLevel, 40.0, rng_);

  nr_pdcch_blind_ul_result_t out;
  ASSERT_TRUE(nr_pdcch_blind_decode_and_extract_01(llr.data(), kAggregationLevel, len, &o, 0x0001, 0xFFEF, &out))
      << (out.reject_reason ? out.reject_reason : "");
  EXPECT_EQ(out.rnti, gt.rnti);
  EXPECT_EQ(out.ul_dci_format, NR_BLIND_UL_DCI_FORMAT_0_1);
  EXPECT_EQ(out.freq_domain_assignment, gt.riv);
  EXPECT_EQ(out.mcs, gt.mcs);
  EXPECT_EQ(out.ndi, gt.ndi);
  EXPECT_EQ(out.rv, gt.rv);
  EXPECT_EQ(out.harq_pid, gt.harq_pid);
  EXPECT_EQ(out.tpc, gt.tpc);
  EXPECT_EQ(out.dai, gt.dai);
  EXPECT_EQ(out.antenna_ports_field, gt.antenna_ports);
  EXPECT_EQ(out.tda_index, gt.tda_index);
  EXPECT_EQ(out.ulsch_indicator, 1);
  // TDRA entry 0 of the configured list, and the k2 that makes the grant actionable.
  EXPECT_EQ(out.start_symbol, 0);
  EXPECT_EQ(out.num_symbols, 14);
  EXPECT_EQ(out.k2, 4);
  EXPECT_EQ(out.mapping_type, 0);
  // Identities fall back to the PCI when not separately configured.
  EXPECT_EQ(out.data_scrambling_id, 2);
  EXPECT_EQ(out.ul_dmrs_scrambling_id, 2);
  EXPECT_EQ(out.transform_precoding, 0);
}

// The two quantities below ARE pinned against the live gNB, unlike the field widths. The FAPI
// UL_TTI.request dump for this cell reads, verbatim:
//   ul_dmrs_symb_pos=2180 dmrs_type=1 nscid=0 dmrs_ports=1 symb=[0..14)
//   num_dmrs_cdm_grps_no_data=2   (and the UL PDCCH line logs `ant=2`)
// 2180 == 0x884 == symbols 2, 7 and 11.
TEST_F(BlindPdcchTest, Dci01DmrsMaskMatchesTheLiveGnbDump) {
  nr_pdcch_blind_ul_opts_t o = LiveUlOpts();
  const uint16_t len = nr_pdcch_blind_dci01_size(&o);

  UlGroundTruth gt;
  gt.tda_index     = 0;  // S=0, L=14, mapping type A
  gt.antenna_ports = 2;
  auto llr = EncodeToLLR(PackUlPayload(gt, o), gt.rnti, len, kAggregationLevel, 40.0, rng_);

  nr_pdcch_blind_ul_result_t out;
  ASSERT_TRUE(nr_pdcch_blind_decode_and_extract_01(llr.data(), kAggregationLevel, len, &o, 0x0001, 0xFFEF, &out))
      << (out.reject_reason ? out.reject_reason : "");
  EXPECT_EQ(out.ul_dmrs_symb_pos, 0x884) << "expected the live gNB's ul_dmrs_symb_pos=2180";
  EXPECT_EQ(out.dmrs_config_type, 0);
  EXPECT_EQ(out.nscid, 0);
}

TEST_F(BlindPdcchTest, Dci01AntennaPortsCodePointMatchesTheLiveGnbDump) {
  nr_pdcch_blind_ul_opts_t o = LiveUlOpts();
  const uint16_t len = nr_pdcch_blind_dci01_size(&o);

  // val=2 -> 2 CDM groups without data, DM-RS port 0 (bitmask 1). Exactly what the gNB dumps.
  UlGroundTruth gt;
  gt.antenna_ports = 2;
  auto llr = EncodeToLLR(PackUlPayload(gt, o), gt.rnti, len, kAggregationLevel, 40.0, rng_);
  nr_pdcch_blind_ul_result_t out;
  ASSERT_TRUE(nr_pdcch_blind_decode_and_extract_01(llr.data(), kAggregationLevel, len, &o, 0x0001, 0xFFEF, &out));
  EXPECT_EQ(out.n_dmrs_cdm_groups, 2);
  EXPECT_EQ(out.dmrs_ports, 1);

  // val=0 is the other branch of the same closed form: 1 CDM group, port 0. Included so a future
  // edit cannot collapse the two cases into one and still pass.
  gt.antenna_ports = 0;
  llr = EncodeToLLR(PackUlPayload(gt, o), gt.rnti, len, kAggregationLevel, 40.0, rng_);
  ASSERT_TRUE(nr_pdcch_blind_decode_and_extract_01(llr.data(), kAggregationLevel, len, &o, 0x0001, 0xFFEF, &out));
  EXPECT_EQ(out.n_dmrs_cdm_groups, 1);
  EXPECT_EQ(out.dmrs_ports, 1);
}

TEST_F(BlindPdcchTest, Dci01RejectsTheDownlinkFormatIndicator) {
  nr_pdcch_blind_ul_opts_t o = LiveUlOpts();
  const uint16_t len = nr_pdcch_blind_dci01_size(&o);
  UlGroundTruth gt;
  gt.format_ind = 1; // a DL assignment that happened to decode at this width
  auto llr = EncodeToLLR(PackUlPayload(gt, o), gt.rnti, len, kAggregationLevel, 40.0, rng_);

  nr_pdcch_blind_ul_result_t out;
  EXPECT_FALSE(nr_pdcch_blind_decode_and_extract_01(llr.data(), kAggregationLevel, len, &o, 0x0001, 0xFFEF, &out));
  EXPECT_FALSE(out.plausible);
  ASSERT_NE(out.reject_reason, nullptr);
}

// UL-SCH indicator 0 is a CSI-only grant: correctly decoded, but it carries no transport block and
// its slot offset is the CSI report's reportSlotOffset, which is NOT in the payload. Accepting it
// would hand a downstream consumer a k2 that was never derived.
TEST_F(BlindPdcchTest, Dci01RejectsACsiOnlyGrant) {
  nr_pdcch_blind_ul_opts_t o = LiveUlOpts();
  const uint16_t len = nr_pdcch_blind_dci01_size(&o);
  UlGroundTruth gt;
  gt.ulsch_ind = 0;
  auto llr = EncodeToLLR(PackUlPayload(gt, o), gt.rnti, len, kAggregationLevel, 40.0, rng_);

  nr_pdcch_blind_ul_result_t out;
  EXPECT_FALSE(nr_pdcch_blind_decode_and_extract_01(llr.data(), kAggregationLevel, len, &o, 0x0001, 0xFFEF, &out));
  ASSERT_NE(out.reject_reason, nullptr);
}

// The reconciliation instrument: the raw payload and the CRC-recovered RNTI must survive a
// rejection, because a rejected payload is exactly what gets replayed against the gNB's log while
// the width assignment is still being pinned.
TEST_F(BlindPdcchTest, Dci01RawPayloadSurvivesRejection) {
  nr_pdcch_blind_ul_opts_t o = LiveUlOpts();
  const uint16_t len = nr_pdcch_blind_dci01_size(&o);
  UlGroundTruth gt;
  gt.ulsch_ind = 0; // rejected, but decoded cleanly
  const uint64_t payload = PackUlPayload(gt, o);
  auto llr = EncodeToLLR(payload, gt.rnti, len, kAggregationLevel, 40.0, rng_);

  nr_pdcch_blind_ul_result_t out;
  ASSERT_FALSE(nr_pdcch_blind_decode_and_extract_01(llr.data(), kAggregationLevel, len, &o, 0x0001, 0xFFEF, &out));
  EXPECT_EQ(out.raw_payload, payload);
  EXPECT_EQ(out.crc_rnti, gt.rnti);
  EXPECT_EQ(out.dci_length, len);
}

TEST_F(BlindPdcchTest, Dci01TdaWidthIsDerivedFromTheListCount) {
  nr_pdcch_blind_ul_opts_t o = LiveUlOpts();
  const uint16_t two_entries = nr_pdcch_blind_dci01_size(&o); // ceil(log2(2)) = 1
  o.tda_count = 5;                                            // ceil(log2(5)) = 3
  o.tda_start[2] = 0; o.tda_length[2] = 10; o.tda_mapping[2] = 0; o.tda_k2[2] = 4;
  o.tda_start[3] = 2; o.tda_length[3] = 10; o.tda_mapping[3] = 1; o.tda_k2[3] = 5;
  o.tda_start[4] = 4; o.tda_length[4] = 8;  o.tda_mapping[4] = 1; o.tda_k2[4] = 6;
  EXPECT_EQ(nr_pdcch_blind_dci01_size(&o), two_entries + 2);

  // ...and the extractor moves with it, which is the property that matters: a width the size
  // function and the packer agree on but the extractor does not is the exact DL bug this mirrors.
  const uint16_t len = nr_pdcch_blind_dci01_size(&o);
  UlGroundTruth gt;
  gt.tda_index = 3;
  auto llr = EncodeToLLR(PackUlPayload(gt, o), gt.rnti, len, kAggregationLevel, 40.0, rng_);
  nr_pdcch_blind_ul_result_t out;
  ASSERT_TRUE(nr_pdcch_blind_decode_and_extract_01(llr.data(), kAggregationLevel, len, &o, 0x0001, 0xFFEF, &out))
      << (out.reject_reason ? out.reject_reason : "");
  EXPECT_EQ(out.tda_index, 3);
  EXPECT_EQ(out.start_symbol, 2);
  EXPECT_EQ(out.num_symbols, 10);
  EXPECT_EQ(out.mapping_type, 1);
  EXPECT_EQ(out.k2, 5);
}

TEST_F(BlindPdcchTest, Dci01RejectsATdaIndexBeyondTheList) {
  nr_pdcch_blind_ul_opts_t o = LiveUlOpts(); // 2 entries -> 1 bit, so no index can overflow...
  o.tda_count = 3;                           // ...but 3 entries -> 2 bits, and code point 3 cannot.
  o.tda_start[2] = 0; o.tda_length[2] = 10; o.tda_mapping[2] = 0; o.tda_k2[2] = 4;
  const uint16_t len = nr_pdcch_blind_dci01_size(&o);
  UlGroundTruth gt;
  gt.tda_index = 3;
  auto llr = EncodeToLLR(PackUlPayload(gt, o), gt.rnti, len, kAggregationLevel, 40.0, rng_);
  nr_pdcch_blind_ul_result_t out;
  EXPECT_FALSE(nr_pdcch_blind_decode_and_extract_01(llr.data(), kAggregationLevel, len, &o, 0x0001, 0xFFEF, &out));
  ASSERT_NE(out.reject_reason, nullptr);
}

// Rather than silently assuming rank 1, a present precoding-information field is rejected: mapping
// its code point to (layers, TPMI) needs a PUSCH-Config this receiver cannot read, and guessing
// would give a confident wrong DM-RS port set and TBS on every multi-layer grant.
// The guard keys on the precoding field's VALUE, not on the field existing. Code point 0 is
// "1 layer, TPMI 0" in every one of TS 38.212 Tables 7.3.1.1.2-2..5 -- whatever the antenna-port
// count, maxRank or codebookSubset -- so it needs no PUSCH-Config and must be ACCEPTED.
//
// This is a regression test for a real live failure: keying on presence rejected 18514 of 18614
// correctly decoded UL grants on a cell that logs mimo=0 on 100 % of its UL DCIs, while every other
// decoded field on those same grants already matched the gNB exactly.
TEST_F(BlindPdcchTest, Dci01AcceptsAPresentButZeroPrecodingField) {
  nr_pdcch_blind_ul_opts_t o = LiveUlOpts();
  o.precoding_info_bits = 1;
  o.antenna_ports_bits  = 2;
  const uint16_t len = nr_pdcch_blind_dci01_size(&o);
  UlGroundTruth gt;
  gt.antenna_ports = 2;
  auto llr = EncodeToLLR(PackUlPayload(gt, o), gt.rnti, len, kAggregationLevel, 40.0, rng_);
  nr_pdcch_blind_ul_result_t out;
  ASSERT_TRUE(nr_pdcch_blind_decode_and_extract_01(llr.data(), kAggregationLevel, len, &o, 0x0001, 0xFFEF, &out))
      << (out.reject_reason ? out.reject_reason : "");
  EXPECT_EQ(out.nrOfLayers, 1);
  EXPECT_EQ(out.precoding_info, 0);
}

// A NON-ZERO code point still needs the tables, so it is still rejected rather than guessed.
TEST_F(BlindPdcchTest, Dci01RejectsANonZeroPrecodingCodePoint) {
  nr_pdcch_blind_ul_opts_t o = LiveUlOpts();
  o.precoding_info_bits = 2;
  const uint16_t len = nr_pdcch_blind_dci01_size(&o);
  UlGroundTruth gt;
  auto llr = EncodeToLLR(PackUlPayload(gt, o), gt.rnti, len, kAggregationLevel, 40.0, rng_);
  nr_pdcch_blind_ul_result_t out;
  // PackUlPayload writes 0 into precoding, so build a non-zero one by hand: the field sits between
  // the SRI and the antenna ports, and with sri = 0 bits it is the 2 bits directly above them.
  const int riv_bits = RivBitsFor(o.bwp_size);
  (void)riv_bits;
  uint64_t p = PackUlPayload(gt, o);
  const int ant_bits = 2;
  const int below = ant_bits + 2 /*srs*/ + 1 /*dmrs seq*/ + 1 /*ulsch*/;
  p |= (uint64_t)1 << below; // set the low bit of the precoding field
  llr = EncodeToLLR(p, gt.rnti, len, kAggregationLevel, 40.0, rng_);
  EXPECT_FALSE(nr_pdcch_blind_decode_and_extract_01(llr.data(), kAggregationLevel, len, &o, 0x0001, 0xFFEF, &out));
  ASSERT_NE(out.reject_reason, nullptr);
}

TEST_F(BlindPdcchTest, Dci01RejectsTheReservedUlMcsRange) {
  nr_pdcch_blind_ul_opts_t o = LiveUlOpts(); // mcs_table = 0 (qam64) -> 28..31 reserved
  const uint16_t len = nr_pdcch_blind_dci01_size(&o);
  UlGroundTruth gt;
  gt.mcs = 29;
  auto llr = EncodeToLLR(PackUlPayload(gt, o), gt.rnti, len, kAggregationLevel, 40.0, rng_);
  nr_pdcch_blind_ul_result_t out;
  EXPECT_FALSE(nr_pdcch_blind_decode_and_extract_01(llr.data(), kAggregationLevel, len, &o, 0x0001, 0xFFEF, &out));
  ASSERT_NE(out.reject_reason, nullptr);
}

// ---- DCI format 0_0. No second polar decode: it reinterprets a payload the 1_0 scan already
// produced, which is what TS 38.212 7.3.1.0's size alignment buys. ----

/// Pack a DCI 0_0 payload at `dci_length`, MSB-first, per TS 38.212 7.3.1.1.1. Any width above the
/// spec field list is trailing zero padding, exactly as the size-alignment rule requires.
uint64_t PackDci00(const UlGroundTruth& gt, uint16_t bwp_size, uint16_t dci_length, uint32_t padding = 0)
{
  uint64_t p = 0;
  int used = 0;
  auto put = [&](uint32_t val, int nbits) {
    if (nbits == 0) return;
    const uint32_t mask = (nbits >= 32) ? 0xFFFFFFFFu : ((1u << nbits) - 1u);
    p = (p << nbits) | (val & mask);
    used += nbits;
  };
  const int riv_bits = RivBitsFor(bwp_size);
  put(gt.format_ind, 1);
  put(gt.riv, riv_bits);
  put(gt.tda_index, 4);
  put(gt.freq_hopping, 1);
  put(gt.mcs, 5);
  put(gt.ndi, 1);
  put(gt.rv, 2);
  put(gt.harq_pid, 4);
  put(gt.tpc, 2);
  const int pad = (int)dci_length - used;
  if (pad > 0) put(padding, pad);
  return p;
}

TEST_F(BlindPdcchTest, Dci00SizeMatchesTheSpecFormulaAndTheFieldList) {
  // 20 fixed + RIV. Cross-checks the (previously dead) size helper against the packer's own count.
  EXPECT_EQ(nr_pdcch_blind_dci00_size(273, 0), (uint16_t)(20 + RivBitsFor(273)));
  EXPECT_EQ(nr_pdcch_blind_dci00_size(273, 1), (uint16_t)(21 + RivBitsFor(273)));
  EXPECT_EQ(nr_pdcch_blind_dci00_size(0, 0), 0);
}

TEST_F(BlindPdcchTest, Dci00ExtractsEveryFieldWithoutASecondDecode) {
  nr_pdcch_blind_ul_opts_t o = LiveUlOpts();
  o.tda_count = 0; // 0_0's TDA is always 4 bits and indexes the DEFAULT table
  const uint16_t len = nr_pdcch_blind_dci00_size(o.bwp_size, 0);

  UlGroundTruth gt;
  gt.riv       = 1200;
  gt.tda_index = 0;   // default table row 0: type A, k2 base 0, S=0, L=14
  gt.mcs       = 10;
  gt.ndi       = 1;
  gt.rv        = 0;
  gt.harq_pid  = 5;
  gt.tpc       = 2;
  gt.freq_hopping = 1;

  nr_pdcch_blind_ul_result_t out;
  ASSERT_TRUE(nr_pdcch_blind_extract_00(PackDci00(gt, o.bwp_size, len), len, gt.rnti, &o, &out))
      << (out.reject_reason ? out.reject_reason : "");
  EXPECT_EQ(out.ul_dci_format, NR_BLIND_UL_DCI_FORMAT_0_0);
  EXPECT_EQ(out.rnti, gt.rnti);
  EXPECT_EQ(out.freq_domain_assignment, gt.riv);
  EXPECT_EQ(out.mcs, gt.mcs);
  EXPECT_EQ(out.ndi, gt.ndi);
  EXPECT_EQ(out.rv, gt.rv);
  EXPECT_EQ(out.harq_pid, gt.harq_pid);
  EXPECT_EQ(out.tpc, gt.tpc);
  EXPECT_EQ(out.frequency_hopping, 1);
  EXPECT_EQ(out.start_symbol, 0);
  EXPECT_EQ(out.num_symbols, 14);
  // Default-table k2 = base + j, and j = 1 at mu = 1 (30 kHz) per TS 38.214 6.1.2.1.1.
  EXPECT_EQ(out.k2, 1);
  EXPECT_EQ(out.ulsch_indicator, 1); // 0_0 has no indicator field; it always schedules UL-SCH
  EXPECT_EQ(out.nscid, 0);           // TS 38.211 6.4.1.1.1
}

TEST_F(BlindPdcchTest, Dci00RejectsTheDownlinkFormatIndicator) {
  nr_pdcch_blind_ul_opts_t o = LiveUlOpts();
  o.tda_count = 0;
  const uint16_t len = nr_pdcch_blind_dci00_size(o.bwp_size, 0);
  UlGroundTruth gt;
  gt.format_ind = 1; // this is a 1_0 payload, not 0_0
  nr_pdcch_blind_ul_result_t out;
  EXPECT_FALSE(nr_pdcch_blind_extract_00(PackDci00(gt, o.bwp_size, len), len, gt.rnti, &o, &out));
  ASSERT_NE(out.reject_reason, nullptr);
}

TEST_F(BlindPdcchTest, Dci00RejectsNonZeroSizeAlignmentPadding) {
  nr_pdcch_blind_ul_opts_t o = LiveUlOpts();
  o.tda_count = 0;
  // Size-aligned UP to a wider 1_0: the excess must be zero (TS 38.212 7.3.1.0).
  const uint16_t len = (uint16_t)(nr_pdcch_blind_dci00_size(o.bwp_size, 0) + 3);
  UlGroundTruth gt;
  nr_pdcch_blind_ul_result_t out;
  EXPECT_TRUE(nr_pdcch_blind_extract_00(PackDci00(gt, o.bwp_size, len, 0), len, gt.rnti, &o, &out))
      << (out.reject_reason ? out.reject_reason : "");
  EXPECT_FALSE(nr_pdcch_blind_extract_00(PackDci00(gt, o.bwp_size, len, 5), len, gt.rnti, &o, &out));
  ASSERT_NE(out.reject_reason, nullptr);
}

// TS 38.214 6.2.2: a 0_0-scheduled PUSCH uses port 0, with one CDM group without data for a
// 2-symbol allocation and two otherwise -- the same length-dependent rule the DL 1_0 path applies.
TEST_F(BlindPdcchTest, Dci00TwoSymbolAllocationUsesOneCdmGroup) {
  nr_pdcch_blind_ul_opts_t o = LiveUlOpts();
  o.tda_count = 2;
  o.tda_start[0] = 0; o.tda_length[0] = 14; o.tda_mapping[0] = 0; o.tda_k2[0] = 4;
  o.tda_start[1] = 4; o.tda_length[1] = 2;  o.tda_mapping[1] = 1; o.tda_k2[1] = 4;
  const uint16_t len = nr_pdcch_blind_dci00_size(o.bwp_size, 0);

  UlGroundTruth gt;
  gt.tda_index = 0; // 14 symbols
  nr_pdcch_blind_ul_result_t out;
  ASSERT_TRUE(nr_pdcch_blind_extract_00(PackDci00(gt, o.bwp_size, len), len, gt.rnti, &o, &out));
  EXPECT_EQ(out.n_dmrs_cdm_groups, 2);

  gt.tda_index = 1; // 2 symbols
  ASSERT_TRUE(nr_pdcch_blind_extract_00(PackDci00(gt, o.bwp_size, len), len, gt.rnti, &o, &out));
  EXPECT_EQ(out.n_dmrs_cdm_groups, 1);
  EXPECT_EQ(out.dmrs_ports, 1);
}

TEST_F(BlindPdcchTest, Dci01PureNoiseFalseAcceptRateIsBounded) {
  nr_pdcch_blind_ul_opts_t o = LiveUlOpts();
  const uint16_t len = nr_pdcch_blind_dci01_size(&o);
  std::normal_distribution<double> noise(0.0, 30.0);

  int accepts = 0;
  const int kTrials = 2000;
  for (int t = 0; t < kTrials; t++) {
    std::vector<int16_t> llr(kAggregationLevel * 108);
    for (auto& v : llr) v = (int16_t)noise(rng_);
    nr_pdcch_blind_ul_result_t out;
    if (nr_pdcch_blind_decode_and_extract_01(llr.data(), kAggregationLevel, len, &o, 0x0001, 0xFFEF, &out)) {
      accepts++;
    }
  }
  // Same bound as the DL groups. Adding a second format must not raise the noise floor -- the
  // live 1_0 rollout measured zero false accepts over 234,000 occasions, and this is the offline
  // counterpart of that check.
  EXPECT_LT(accepts, kTrials / 20) << "accepts=" << accepts << " of " << kTrials;
}

// ---- Phase 1: CSS0 self-configuration ---------------------------------------------------------
// The claim under test is not "autoconf sets the CORESET#0 constants" (it plainly does) but
// "autoconf switches OFF the settings that describe the DEDICATED search space". Leaving one on
// does active harm rather than nothing: dci01_scan consumed every surviving candidate and rejected
// all of them, and the adaptive energy gate rejected 100% of candidates forever (measured
// 2026-09-04: candidates frozen at 199 == ENERGY_FLOOR_WARMUP while held[energy] grew 3/occasion).
TEST(Css0Autoconf, TurnsOffEverySettingThatDescribesTheDedicatedSearchSpace) {
  auto* c = const_cast<nr_pdcch_blind_monitor_cfg_t*>(nr_pdcch_blind_monitor_get_cfg());

  // What nrue.passive_rx.autoconf.conf actually sets today for the dedicated CORESET.
  c->energy_adapt_factor = 3.0f;
  c->energy_min          = 2.0f;
  c->dci01_scan          = 1;
  c->rnti_min            = 1;
  c->rnti_max            = 0xFFEF;

  // This cell: CORESET#0 = 48 RB / 1 symbol at CRB 0, SSB at CRB offset 12 from point A,
  // SS0 period 40 slots / offset 0 / duration 2 / first symbol 0, mux pattern 1, PCI 2,
  // rb_offset 12 (so cset_start_rb = ssb_offset_point_a - rb_offset = 0 is self-consistent),
  // dmrs-TypeA-Position 2.
  ASSERT_TRUE(nr_pdcch_blind_monitor_autoconf_css0(48, 1, 0, 12, 40, 0, 2, 0, 1, 2, 12, 2));

  // The adaptive energy floor is estimated from the very candidates it gates. On the dedicated
  // CORESET (45 groups) most candidates are empty so it tracks noise; CORESET#0 is 8 CCEs with
  // SIB1 every 20 ms, so every sample is signal and the floor rises to meet it.
  EXPECT_FLOAT_EQ(c->energy_adapt_factor, 0.0f);
  EXPECT_FLOAT_EQ(c->energy_min, 0.0f);

  // Already-established behaviour, asserted here so a future edit cannot silently drop it.
  EXPECT_EQ(c->dci01_scan, 0);
  EXPECT_EQ(c->rnti_min, 0xFFFF);
  EXPECT_EQ(c->rnti_max, 0xFFFF);

  // The autoconf zero-config path must set the one MIB-derivable field it previously left at its
  // illegal zero default (spec values are 2 or 3) -- feeds the passive PDSCH-extraction l0/DM-RS
  // mask (blind_fill_dmrs_mask()).
  EXPECT_EQ(c->dmrs_typeA_position, 2);
}

// BWPStart is the frequency origin for both the DM-RS sequence and the RIV. A wrong value leaves
// every log line looking healthy, so the value must carry its derivation rather than be copied: it
// is cset_start_rb, which nr_mac_common.c:4078 defines as ssb_offset_point_a - rb_offset, and which
// OAI's own working normal path uses for coreset_id == 0 (nr_ue_dci_configuration.c:225).
TEST(Css0Autoconf, BwpOriginIsTheCoresetZeroStartNotTheSsbOrigin) {
  auto* c = const_cast<nr_pdcch_blind_monitor_cfg_t*>(nr_pdcch_blind_monitor_get_cfg());

  // A cell where the two differ, so passing the wrong one is detectable: CORESET#0 starts at
  // CRB 1 while the SSB sits at CRB 26 (the 2026-08-19 cell: offsetToPointA 26, rb_offset 25).
  ASSERT_TRUE(nr_pdcch_blind_monitor_autoconf_css0(48, 1, 1, 26, 40, 0, 2, 0, 1, 2, 25, 2));
  EXPECT_EQ(c->bwp_start, 1);
  EXPECT_EQ(c->bwp_size, 48);
  EXPECT_EQ(c->coreset_freq_domain, 8);
  EXPECT_EQ(c->coreset_type, 1);
}

}  // namespace

extern "C" {
// Forward-declared directly (not via nr_transport_proto_ue.h, which is full of GNU-C VLA
// parameter-dependent array declarations invalid in C++) -- same signatures that header carries.
void nr_pdcch_demapping_deinterleaving(uint32_t coreset_nbr_rb, c16_t *llr, c16_t *e_rx,
                                       uint8_t coreset_time_dur, uint8_t reg_bundle_size_L_in,
                                       uint8_t coreset_interleaver_size_R, uint8_t n_shift,
                                       uint8_t number_of_candidates, uint16_t *CCE, uint8_t *L,
                                       int llr_stride_per_symbol);
void nr_pdcch_unscrambling(c16_t *e_rx, uint16_t scrambling_RNTI, uint32_t length,
                          uint16_t pdcch_DMRS_scrambling_id, int16_t *z2);
}

// Stubs for symbols reachable ONLY through dci_nr.c's nr_rx_pdcch_symbol() (the legacy live
// PDCCH-symbol-processing entry point), which this test never calls -- only
// nr_pdcch_demapping_deinterleaving()/nr_pdcch_unscrambling() are actually exercised below. Real
// implementations pull in heavy PHY estimation/FFT dependencies for code this test never runs;
// same sidestep convention this file already uses for get_softmodem_params()/uniqCfg above.
extern "C" {
void nr_pdcch_dmrs_ref(void) {}
void nr_pdcch_channel_estimation(void) {}
void nr_channel_level(void) {}
uint8_t log2_approx(uint32_t x) { (void)x; return 0; }
}


// ---------------------------------------------------------------------------------------------
// Group: interleaved CORESET#0 demapping/deinterleaving + unscrambling, end-to-end.
//
// No existing test in this file (or anywhere in this codebase) exercises
// nr_pdcch_demapping_deinterleaving()/nr_pdcch_unscrambling() at all -- every existing test builds
// dci_estimation[] directly and skips straight to polar_decoder_int16(). This group closes that
// gap for the SPECIFIC config CORESET#0 always uses: interleaved (TS 38.213 Table 10.1-1 mandates
// bundle=6/R=2 for CORESET#0 unconditionally), using this cell's live-verified parameters
// (48 REGs / 8 CCEs, n_shift=PCI=2).
//
// CONSTRUCTION NOTE, load-bearing: coded bits are assigned to a candidate's own bundles in
// ASCENDING PHYSICAL position order, NOT ascending logical-bundle order. This was gotten wrong in
// an earlier version of this test (see git history, commit 34fc1dc904/its revert
// 73b0e34c50) -- the gNB-side TX mapping (openair1/PHY/NR_TRANSPORT/nr_dci_tools.c:56-73,
// "this implements TS 38.211 Sec. 7.3.2.2") builds each candidate's REG list in logical order via
// the interleaver formula, then explicitly qsorts it into ascending PHYSICAL order before
// assigning the coded bit stream -- confirmed against that real gNB-side code, not assumed.
// ---------------------------------------------------------------------------------------------

namespace {

int InterleaverF(int k, int R, int C, int n_shift, int N_over_L) {
  if (R == 0) return k;
  const int c = k / R;
  const int r = k % R;
  return (r * C + c + n_shift) % N_over_L;
}

/// Builds a physical-domain, interleaved candidate the way a gNB transmitting on CORESET#0
/// actually does: encode -> "scramble" (nr_pdcch_unscrambling() is a pure XOR-with-gold-sequence
/// sign flip, so calling it ONCE on the clean values produces exactly what would be transmitted)
/// -> assign the coded bit stream to the candidate's own logical bundles taken in ASCENDING
/// PHYSICAL position order (matching nr_dci_tools.c's qsort), each placed at ITS OWN physical
/// bundle position in the returned buffer.
std::vector<c16_t> BuildInterleavedCandidateLlr(uint64_t packed, uint16_t rnti, uint16_t dci_length,
                                                uint8_t agg_level, uint16_t dmrs_scrambling_id,
                                                int n_rb_coreset, int reg_bundle_L, int interleaver_R,
                                                int n_shift) {
  t_nrPolar_params* params = nr_polar_params(NR_POLAR_DCI_MESSAGE_TYPE, dci_length, agg_level);
  const uint16_t encoder_len = params->encoderLength;

  std::vector<uint32_t> out((encoder_len + 31) / 32, 0);
  polar_encoder_fast(&packed, out.data(), (int32_t)rnti, /*ones_flag=*/1, NR_POLAR_DCI_MESSAGE_TYPE,
                     dci_length, agg_level);
  std::vector<uint8_t> coded_bits(encoder_len);
  nr_bit2byte_uint32_8(out.data(), encoder_len, coded_bits.data());

  // "clean" (pre-scramble) soft representation: bit 0 -> +K, bit 1 -> -K, matching this file's
  // own EncodeToLLR() convention and matching what a correct nr_pdcch_unscrambling() call recovers.
  const int16_t K = 100;
  std::vector<int16_t> clean(encoder_len);
  for (int i = 0; i < encoder_len; i++) clean[i] = (coded_bits[i] == 0) ? K : (int16_t)(-K);

  std::vector<int16_t> tx(encoder_len);
  nr_pdcch_unscrambling(reinterpret_cast<c16_t*>(clean.data()), rnti, encoder_len, dmrs_scrambling_id,
                        tx.data());

  const int N_regs = n_rb_coreset; // duration 1
  const int B_rb = reg_bundle_L;   // duration 1
  const int max_bundles = N_regs / reg_bundle_L;
  const int C = N_regs / (interleaver_R * reg_bundle_L);
  const int re_per_bundle = B_rb * 9; // RE_PER_RB_OUT_DMRS == 9, this module's own #define

  std::vector<c16_t> llr((size_t)N_regs * 9, {0, 0});

  // agg_level CCEs = agg_level logical bundles for this cell (bundle size 6 == REGs/CCE). Compute
  // each one's physical position, then SORT ascending by physical position -- this is the qsort
  // step nr_dci_tools.c performs on its own reg_list before bit assignment.
  std::vector<int> phys_positions(agg_level);
  for (int cce = 0; cce < agg_level; cce++) {
    phys_positions[cce] = InterleaverF(cce, interleaver_R, C, n_shift, max_bundles);
  }
  std::vector<int> order(agg_level);
  for (int i = 0; i < agg_level; i++) order[i] = i;
  std::sort(order.begin(), order.end(),
           [&](int a, int b) { return phys_positions[a] < phys_positions[b]; });

  // Walk the SORTED order, handing out consecutive 108-bit chunks of the coded stream in that
  // order -- chunk 0 (the first 108 coded bits) goes to whichever logical bundle has the smallest
  // physical position, chunk 1 to the next-smallest, etc. Each chunk is physically placed at ITS
  // OWN bundle's physical RE range (unchanged from before -- only the STREAM-TO-BUNDLE assignment
  // order changes).
  for (int chunk = 0; chunk < agg_level; chunk++) {
    const int phys_bundle = phys_positions[order[chunk]];
    for (int re = 0; re < re_per_bundle; re++) {
      const int m = chunk * (re_per_bundle * 2) + re * 2;
      c16_t& dst = llr[(size_t)phys_bundle * re_per_bundle + re];
      dst.r = tx[m];
      dst.i = tx[m + 1];
    }
  }
  return llr;
}

}  // namespace

TEST_F(BlindPdcchTest, InterleavedCoreset0CandidateRoundTrips) {
  // This cell's live CORESET#0: 48 REGs (8 CCEs), bundle=6, interleaver R=2, n_shift=PCI=2.
  const int n_rb_coreset = 48, reg_bundle_L = 6, interleaver_R = 2, n_shift = 2;
  const uint8_t agg_level = 4; // this cell's live SI-RNTI aggregation level

  const uint16_t bwp_size = (uint16_t)n_rb_coreset;
  const int riv_bits = RivBitsFor(bwp_size);
  const uint16_t dci_length = 39; // this cell's live-verified DCI 1_0/CORESET#0 length

  GroundTruth gt;
  gt.rnti = 0xFFFF; // SI-RNTI
  gt.bwp_size = bwp_size;
  gt.riv = 12345u % (1u << riv_bits);
  const uint64_t packed = PackPayload(gt, riv_bits);

  auto llr = BuildInterleavedCandidateLlr(packed, gt.rnti, dci_length, agg_level,
                                          /*dmrs_scrambling_id=*/2, n_rb_coreset, reg_bundle_L,
                                          interleaver_R, n_shift);

  uint16_t cce_list[1] = {0};
  uint8_t l_list[1] = {agg_level};
  c16_t e_rx[NR_MAX_PDCCH_SIZE];
  nr_pdcch_demapping_deinterleaving((uint32_t)n_rb_coreset, llr.data(), e_rx,
                                    /*coreset_time_dur=*/1, (uint8_t)reg_bundle_L,
                                    (uint8_t)interleaver_R, (uint8_t)n_shift,
                                    /*number_of_candidates=*/1, cce_list, l_list,
                                    /*llr_stride_per_symbol=*/(int)llr.size());

  int16_t tmp_e[16 * 108];
  nr_pdcch_unscrambling(e_rx, gt.rnti, agg_level * 108, /*dmrs_scrambling_id=*/2, tmp_e);

  uint64_t dci_estimation[2] = {0};
  const uint32_t crc = polar_decoder_int16(tmp_e, dci_estimation, 1, NR_POLAR_DCI_MESSAGE_TYPE,
                                          dci_length, agg_level);

  ASSERT_EQ(crc, gt.rnti) << "interleaved CORESET#0 candidate did not round-trip through the real "
                             "demapping/deinterleaving + unscrambling chain";
  EXPECT_EQ(dci_estimation[0] & ((1ULL << dci_length) - 1), packed & ((1ULL << dci_length) - 1))
      << "only the low dci_length bits are meaningful -- higher bits are PackPayload overflow "
         "from an oversized synthetic field set, not part of the actual encoded/decoded payload";
}

// ---- Extent candidate enumeration (Phase 3 Technique A, 2026-09-07) -------------------------
// The true CORESET must CONTAIN every observed window, and candidate 0 must stay the legacy
// snap-to-carrier answer so a cell where that was already right cannot regress.

TEST(ExtentCandidates, FirstCandidateIsTheLegacySnapAnswer) {
  // This cell: 45 windows, occupancy observed over [0..43] -> the legacy rule snaps to the carrier.
  nr_pdcch_extent_cand_t c[8];
  const int n = nr_pdcch_extent_candidates(0, 43, 45, c, 8);
  ASSERT_GE(n, 1);
  EXPECT_EQ(c[0].first_w, 0);
  EXPECT_EQ(c[0].last_w, 44);  // snapped, i.e. span_rb = 270
  // Exactly the two hypotheses the histogram admits: the snap, and the observed extent itself.
  EXPECT_EQ(n, 2);
  EXPECT_EQ(c[1].last_w, 43);
}

TEST(ExtentCandidates, EveryCandidateContainsTheObservedFootprint) {
  nr_pdcch_extent_cand_t c[8];
  const int n = nr_pdcch_extent_candidates(2, 5, 20, c, 8);
  ASSERT_GT(n, 0);
  for (int i = 0; i < n; i++) {
    EXPECT_LE(c[i].first_w, 2) << "candidate " << i << " excludes an observed window";
    EXPECT_GE(c[i].last_w, 5) << "candidate " << i << " excludes an observed window";
    EXPECT_LT(c[i].last_w, 20);
  }
}

TEST(ExtentCandidates, NoDuplicates) {
  nr_pdcch_extent_cand_t c[8];
  const int n = nr_pdcch_extent_candidates(0, 43, 45, c, 8);
  for (int i = 0; i < n; i++) {
    for (int j = i + 1; j < n; j++) {
      EXPECT_FALSE(c[i].first_w == c[j].first_w && c[i].last_w == c[j].last_w);
    }
  }
}

TEST(ExtentCandidates, OffsetIsDeliberatelyNotSwept) {
  // Sweeping first_w would emit geometries this module cannot currently APPLY: the FAPI builder
  // hardcodes coreset.rb_offset = 0, so a nonzero offset is carried through BWPStart and moves the
  // BWP frame instead. Assert the restriction so removing it is a deliberate act, not a slip.
  nr_pdcch_extent_cand_t c[8];
  const int n = nr_pdcch_extent_candidates(3, 6, 20, c, 8);
  ASSERT_GT(n, 0);
  for (int i = 0; i < n; i++) {
    EXPECT_EQ(c[i].first_w, 3);
  }
}

TEST(ExtentCandidates, RejectsInvalidInputAndRespectsTheCap) {
  nr_pdcch_extent_cand_t c[8];
  EXPECT_EQ(nr_pdcch_extent_candidates(5, 3, 45, c, 8), 0);   // last before first
  EXPECT_EQ(nr_pdcch_extent_candidates(0, 45, 45, c, 8), 0);  // last outside the carrier
  EXPECT_EQ(nr_pdcch_extent_candidates(0, 43, 45, c, 0), 0);  // no room
  EXPECT_EQ(nr_pdcch_extent_candidates(0, 0, 64, c, 3), 3);   // capped, not overrun
}

int main(int argc, char** argv)
{
  logInit();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
