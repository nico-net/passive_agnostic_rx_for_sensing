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

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
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

}  // namespace

int main(int argc, char** argv)
{
  logInit();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
