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
#include <time.h>

#include <gtest/gtest.h>

extern "C" {
void crcTableInit(void);
#include "PHY/CODING/nrPolar_tools/nr_polar_dci_defs.h"
#include "PHY/CODING/nrPolar_tools/nr_polar_defs.h"
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
#include "common/utils/nr/nr_common.h"
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h" // NR_tda_info_t, get_dl_tda_info(), TYPE_C_RNTI_
#include "nr_pdcch_blind_monitor.h"
#include "nr_pdsch_config_sweep.h"
#include "nr_pdcch_coreset_map.h"
#include "nr_pdcch_discovery_replay.h"
#include "nr_pdcch_dci_length_sweep.h"
#include "nr_pdcch_ul_field_sweep.h"
#include "nr_pdcch_ul_discovery.h"
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
  polarReturn(params);

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
// Group 0b: the GPU batch hand-off (nr_pdcch_blind_polar_pre_set, nr_polar_gpu.h). The batch path
// replaces ONLY the decode, so the one thing that has to hold is that a hand-off is consumed when
// it belongs to this candidate and IGNORED otherwise -- a wrongly consumed one would attribute
// another candidate's payload to this one, silently, with every gate downstream still believing it.
// ---------------------------------------------------------------------------------------------
TEST_F(BlindPdcchTest, PolarPreHandoffIsConsumedOnlyWhenItMatches) {
  const uint16_t bwp_size = 106;
  const uint16_t dci_length = nr_pdcch_blind_dci_size(bwp_size);

  GroundTruth gt;
  gt.rnti = 0x4601;
  gt.bwp_size = bwp_size;
  gt.riv = 7;
  gt.time_domain_assignment = 1;
  gt.antenna_ports = 5;
  gt.dmrs_seq_init = 1;
  const uint64_t packed = PackPayload(gt, RivBitsFor(bwp_size));
  auto llr = EncodeToLLR(packed, gt.rnti, dci_length, kAggregationLevel, /*snr_db=*/40.0, rng_);

  // Baseline: the CPU decode this candidate would do on its own.
  nr_pdcch_blind_raw_result_t cpu = {};
  ASSERT_TRUE(nr_pdcch_blind_decode_raw_11(llr.data(), kAggregationLevel, dci_length, 0x0001, 0xFFEF, &cpu));

  nr_pdcch_blind_polar_pre_t pre = {};
  pre.llr = llr.data();
  pre.crc = cpu.rnti;
  pre.payload = cpu.payload;
  pre.dci_length = dci_length;
  pre.aggregation_level = kAggregationLevel;

  // Matching hand-off: same answer, and consumed (the second decode must fall back to the CPU and
  // still agree -- which is also what makes a decode path that decodes twice safe).
  nr_pdcch_blind_polar_pre_set(&pre);
  nr_pdcch_blind_raw_result_t gpu = {};
  ASSERT_TRUE(nr_pdcch_blind_decode_raw_11(llr.data(), kAggregationLevel, dci_length, 0x0001, 0xFFEF, &gpu));
  EXPECT_EQ(gpu.rnti, cpu.rnti);
  EXPECT_EQ(gpu.payload, cpu.payload);
  EXPECT_EQ(gpu.mismatched_bits, cpu.mismatched_bits);

  nr_pdcch_blind_raw_result_t again = {};
  ASSERT_TRUE(nr_pdcch_blind_decode_raw_11(llr.data(), kAggregationLevel, dci_length, 0x0001, 0xFFEF, &again));
  EXPECT_EQ(again.payload, cpu.payload) << "one-shot hand-off must not survive into a second decode";

  // Mismatched hand-off (another candidate's payload under a wrong length/AL/buffer): must be
  // ignored, not returned.
  const uint64_t poison = packed ^ 0x2AAAAAAAull;
  for (int k = 0; k < 3; k++) {
    nr_pdcch_blind_polar_pre_t bad = pre;
    bad.crc = 0x1234;
    bad.payload = poison;
    if (k == 0)
      bad.dci_length = (uint16_t)(dci_length - 1);
    else if (k == 1)
      bad.aggregation_level = (uint8_t)(kAggregationLevel * 2);
    else
      bad.llr = llr.data() + 1;
    nr_pdcch_blind_polar_pre_set(&bad);
    nr_pdcch_blind_raw_result_t r = {};
    ASSERT_TRUE(nr_pdcch_blind_decode_raw_11(llr.data(), kAggregationLevel, dci_length, 0x0001, 0xFFEF, &r)) << k;
    EXPECT_EQ(r.payload, cpu.payload) << "case " << k << ": a non-matching hand-off must fall back to the CPU decode";
    EXPECT_EQ(r.rnti, cpu.rnti) << "case " << k;
  }
  nr_pdcch_blind_polar_pre_set(nullptr);
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
  EXPECT_STREQ(out.reject_reason, "antenna_ports field outside its table's valid rows");
}

/* DM-RS type 2's DCI 1_1 antenna-ports tables (TS 38.212 Table 7.3.1.2.2-3, 5-bit field, maxLength 1;
 * -4, 6-bit field, maxLength 2) were already implemented (g_table_7_3_2_3_3_3/_4) but had NO test
 * exercising them -- this closes that gap. Row values below are read verbatim off the in-tree
 * tables, not derived, so a wrong row constant here would be caught by the table itself disagreeing
 * with the spec, not silently agreeing with a copy-paste mistake. */
TEST_F(BlindPdcchTest, Dci11Type2AntennaPortsDecodeViaTables3And4) {
  const uint16_t bwp_size = 106;
  const int      riv_bits = RivBitsFor(bwp_size);

  // Table 7.3.1.2.2-3 row 6 = {2,0,0,0,1,0,0}: 2 CDM groups without data, port 3 only.
  {
    nr_pdcch_blind_extract_opts_t opts = DefaultOpts();
    opts.dmrs_config_type   = 1;
    opts.antenna_ports_bits = 5;
    const uint16_t len = nr_pdcch_blind_dci_size_ex(bwp_size, &opts);

    GroundTruth gt;
    gt.rnti                   = 0x4A11;
    gt.bwp_size                = bwp_size;
    gt.riv                     = (uint32_t)PRBalloc_to_locationandbandwidth0(20, 10, bwp_size);
    gt.time_domain_assignment  = 0;
    gt.antenna_ports            = 6;

    auto llr = EncodeToLLR(PackPayload(gt, riv_bits, &opts), gt.rnti, len, kAggregationLevel, 40.0, rng_);
    nr_pdcch_blind_result_t out;
    ASSERT_TRUE(nr_pdcch_blind_decode_and_extract_ex(llr.data(), kAggregationLevel, len, bwp_size,
                                                     kDmrsTypeAPositionPos2, NR_PDCCH_BLIND_RNTI_MIN_DEFAULT,
                                                     NR_PDCCH_BLIND_RNTI_MAX_DEFAULT, &opts, &out));
    EXPECT_EQ(out.dmrs_config_type, 1);
    EXPECT_EQ(out.n_dmrs_cdm_groups, 2);
    EXPECT_EQ(out.dmrs_ports, 1u << 3);
  }

  // Table 7.3.1.2.2-4 row 24 = {3,1,0,0,0,0,0,0,0,0,0,0,0,2}: 3 CDM groups, port 0 only,
  // maxLength 2 (front-loaded + 1 additional symbol, the "symbols" column = 2).
  {
    nr_pdcch_blind_extract_opts_t opts = DefaultOpts();
    opts.dmrs_config_type   = 1;
    opts.antenna_ports_bits = 6;
    // g_table_6_4_1_1_3_4 (maxLength 2's DM-RS symbol-mask table) defines ONLY additional
    // positions 0 and 1 for mapping type A -- columns 2/3 are -1 (reserved) in every row, a real
    // TS 38.211 Table 6.4.1.1.3-4 fact (double-symbol front-loading is simply not combined with
    // pos2/pos3), not a bug. DefaultOpts()'s pos2 fallback is invalid here, so pin pos1.
    opts.dmrs_add_pos       = 1;
    const uint16_t len = nr_pdcch_blind_dci_size_ex(bwp_size, &opts);

    GroundTruth gt;
    gt.rnti                   = 0x4A22;
    gt.bwp_size                = bwp_size;
    gt.riv                     = (uint32_t)PRBalloc_to_locationandbandwidth0(20, 10, bwp_size);
    gt.time_domain_assignment  = 0;
    gt.antenna_ports            = 24;

    auto llr = EncodeToLLR(PackPayload(gt, riv_bits, &opts), gt.rnti, len, kAggregationLevel, 40.0, rng_);
    nr_pdcch_blind_result_t out;
    ASSERT_TRUE(nr_pdcch_blind_decode_and_extract_ex(llr.data(), kAggregationLevel, len, bwp_size,
                                                     kDmrsTypeAPositionPos2, NR_PDCCH_BLIND_RNTI_MIN_DEFAULT,
                                                     NR_PDCCH_BLIND_RNTI_MAX_DEFAULT, &opts, &out));
    EXPECT_EQ(out.dmrs_config_type, 1);
    EXPECT_EQ(out.n_dmrs_cdm_groups, 3);
    EXPECT_EQ(out.dmrs_ports, 1u);
  }
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
  polarReturn(params);

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
  EXPECT_EQ(c->dci01_scan, 1); // preserve intent, suppress effective scanning in CSS0
  EXPECT_FALSE(nr_pdcch_blind_monitor_ul_scan_enabled(c));
  // CSS0 admits the whole plausible RNTI range, not just RA-RNTI: MEASURED 2026-09-21 (Swisscom
  // PCI 382) that Msg4 DCIs are scrambled by TC-RNTIs above NR_PDCCH_BLIND_RA_RNTI_MAX (0x4600),
  // so pinning to the RA-RNTI range silently dropped every RRCSetup harvest. See
  // nr_pdcch_blind_monitor.c's autoconf comment (~line 395) for the full "COST OF WIDENING"
  // argument -- SI-RNTI/P-RNTI are admitted by class independently of this range regardless.
  EXPECT_EQ(c->rnti_min, 1);
  EXPECT_EQ(c->rnti_max, NR_PDCCH_BLIND_RNTI_MAX_DEFAULT);

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

// ISAC_CSS0_INTERLEAVE_K swaps the WHOLE config struct per occasion, so what it has to guarantee is
// (a) the snapshot really is the common-search-space one, (b) it carries autodiscover OFF so an
// interleaved occasion cannot advance -- and then have reverted -- a dedicated-sweep hypothesis, and
// (c) the swap is an exact round trip. Field-by-field assertions would just re-list the ~20 fields
// the snapshot exists to avoid listing; memcmp is the actual contract.
TEST(Css0Interleave, SnapshotIsTheCommonConfigAndTheSwapRoundTripsExactly) {
  auto* c = const_cast<nr_pdcch_blind_monitor_cfg_t*>(nr_pdcch_blind_monitor_get_cfg());
  ASSERT_TRUE(nr_pdcch_blind_monitor_autoconf_css0(48, 1, 0, 12, 40, 0, 2, 0, 1, 2, 12, 2));

  const nr_pdcch_blind_monitor_cfg_t* c0 = nr_pdcch_blind_monitor_css0_cfg();
  ASSERT_NE(c0, nullptr);
  EXPECT_EQ(c0->coreset_type, 1);       // CORESET#0, not the dedicated one
  // Deliberately widened 2026-09-21 (Swisscom PCI 382 Msg4-TC-RNTI measurement, see the
  // TurnsOffEverySettingThatDescribesTheDedicatedSearchSpace comment above) to the full plausible
  // RNTI range, not just RA-RNTI.
  EXPECT_EQ(c0->rnti_max, NR_PDCCH_BLIND_RNTI_MAX_DEFAULT);
  EXPECT_EQ(c0->autodiscover, 0);       // must not run the dedicated sweep's bookkeeping

  // Stand in for "autodiscover has since overwritten the live config with the dedicated one".
  c->coreset_type = 0;
  c->rnti_min     = 1;
  c->rnti_max     = 0xFFEF;
  c->autodiscover = 1;
  const nr_pdcch_blind_monitor_cfg_t dedicated = *c;

  // While the override is on, THIS thread reads the common config...
  nr_pdcch_blind_monitor_cfg_override(c0);
  const nr_pdcch_blind_monitor_cfg_t* live = nr_pdcch_blind_monitor_get_cfg();
  EXPECT_EQ(live->coreset_type, 1);
  EXPECT_EQ(live->rnti_max, NR_PDCCH_BLIND_RNTI_MAX_DEFAULT); // see the widening note above
  EXPECT_EQ(live->autodiscover, 0);
  // ...and the dedicated config is UNTOUCHED, which is what keeps a scan consumer on another thread
  // (and the dedicated sweep's own state) out of the interleave's way.
  EXPECT_EQ(memcmp(c, &dedicated, sizeof(dedicated)), 0);

  nr_pdcch_blind_monitor_cfg_override(nullptr);
  EXPECT_EQ(nr_pdcch_blind_monitor_get_cfg(), c);
  EXPECT_EQ(memcmp(c, &dedicated, sizeof(dedicated)), 0);
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
  polarReturn(params);

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

TEST(ExtentCandidates, AWideFootprintIsTriedAsObservedThenAsTheFullCarrier) {
  // The 2026-09-07 live failure: traffic thinned, windows 0-1 fell under the hit floor, and the
  // footprint came out 2..37 (36 of 45 windows = 80 %) against a truth of 0..44. The old answer
  // SNAPPED a wide span to the carrier -- and on the OAI rfsim cell (2026-09-16) that turned a
  // CORRECT 0..39 observation (a 48-RB-quantised 240-of-273 CORESET) into 0..44, which the
  // grow-only walk could never undo: 0 accepts at every candidate. Now the observation is tried
  // as observed and the full carrier is always the SECOND hypothesis, so both cells converge.
  nr_pdcch_extent_cand_t c[8];
  const int n = nr_pdcch_extent_candidates(2, 37, 45, c, 8);
  ASSERT_GE(n, 2);
  EXPECT_EQ(c[0].first_w, 2);
  EXPECT_EQ(c[0].last_w, 37);
  EXPECT_EQ(c[1].first_w, 0);
  EXPECT_EQ(c[1].last_w, 44);
}

TEST(ExtentCandidates, DoesNotSnapANarrowFootprint) {
  // A genuinely narrow CORESET must be left alone; snapping it to the carrier would be the same
  // class of error in the other direction.
  nr_pdcch_extent_cand_t c[8];
  const int n = nr_pdcch_extent_candidates(2, 8, 45, c, 8);
  ASSERT_GE(n, 1);
  EXPECT_EQ(c[0].first_w, 2);
  EXPECT_EQ(c[0].last_w, 8);
}

TEST(ExtentCandidates, TheOaiQuantisedCoresetIsTheFirstHypothesis) {
  // OAI's dedicated CORESET on 273 PRB is 240 RB (windows 0..39). Observed exactly, it must be the
  // first hypothesis; the carrier comes second; the walk then grows.
  nr_pdcch_extent_cand_t c[8];
  const int n = nr_pdcch_extent_candidates(0, 39, 45, c, 8);
  ASSERT_GE(n, 3);
  EXPECT_EQ(c[0].first_w, 0);
  EXPECT_EQ(c[0].last_w, 39);
  EXPECT_EQ(c[1].first_w, 0);
  EXPECT_EQ(c[1].last_w, 44);
  EXPECT_EQ(c[2].last_w, 40);
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

TEST(ExtentCandidates, OffsetIsNowSwept) {
  // The offset is searched, not assumed: dci_nr.c uses cset_start + coreset->rb_offset both for the
  // RE index and to index the PDCCH DM-RS sequence, so a wrong offset yields a wrong pilot sequence
  // and reads as a dead channel. It was previously excluded only because it could not be APPLIED.
  nr_pdcch_extent_cand_t c[8];
  const int n = nr_pdcch_extent_candidates(3, 6, 20, c, 8);
  ASSERT_GT(n, 1);
  bool saw_smaller_offset = false;
  for (int i = 0; i < n; i++) {
    if (c[i].first_w < 3) {
      saw_smaller_offset = true;
    }
  }
  EXPECT_TRUE(saw_smaller_offset) << "offset never varied, so a mis-placed CORESET is unrecoverable";
}

TEST(ExtentCandidates, NearestHypothesesComeFirst) {
  // The histogram is a LOWER BOUND on the footprint and usually a tight one, so the verification
  // dwell must be spent near the observation rather than on the widest geometry.
  nr_pdcch_extent_cand_t c[8];
  const int n = nr_pdcch_extent_candidates(3, 6, 20, c, 8);
  ASSERT_GE(n, 3);
  // Candidate 1 is the FULL CARRIER: the observation is only where DCIs landed in the dwell, and a
  // BWP-wide CORESET is the common configuration (OAI, srsRAN) -- testing it second costs one
  // dwell and saved walking 322 dilations on the rfsim cell (2026-09-16). Nearest-first from there.
  EXPECT_EQ(c[1].first_w, 0);
  EXPECT_EQ(c[1].last_w, 19);
  int prev = -1;
  for (int i = 2; i < n; i++) {
    const int d = (3 - c[i].first_w) + (c[i].last_w - 6);
    EXPECT_GE(d, prev) << "candidate " << i << " is nearer than one tried before it";
    prev = d;
  }
}

TEST(ExtentCandidates, EveryOffsetCandidateStillContainsTheObservation) {
  // Expanding must never EXCLUDE an observed window: those windows carried real PDCCH DM-RS.
  nr_pdcch_extent_cand_t c[8];
  const int n = nr_pdcch_extent_candidates(3, 6, 20, c, 8);
  for (int i = 0; i < n; i++) {
    EXPECT_LE(c[i].first_w, 3);
    EXPECT_GE(c[i].last_w, 6);
    EXPECT_GE(c[i].first_w, 0);
    EXPECT_LT(c[i].last_w, 20);
  }
}

TEST(ExtentCandidates, RejectsInvalidInputAndRespectsTheCap) {
  nr_pdcch_extent_cand_t c[8];
  EXPECT_EQ(nr_pdcch_extent_candidates(5, 3, 45, c, 8), 0);   // last before first
  EXPECT_EQ(nr_pdcch_extent_candidates(0, 45, 45, c, 8), 0);  // last outside the carrier
  EXPECT_EQ(nr_pdcch_extent_candidates(0, 43, 45, c, 0), 0);  // no room
  EXPECT_EQ(nr_pdcch_extent_candidates(0, 0, 64, c, 3), 3);   // capped, not overrun
}

TEST(ExtentCandidates, MultiplePeaksRemainIndependentAndCannotExcludeTruth) {
  const int seeds[] = {0, 13};
  nr_pdcch_extent_cand_t c[36 * 37 / 2];
  const int n = nr_pdcch_extent_candidates_multi(seeds, 2, 36, c, 36 * 37 / 2);
  EXPECT_EQ(n, 36 * 37 / 2);
  bool saw0 = false, saw13 = false, saw_disjoint_truth = false, saw_forced_span_first = false;
  for (int i = 0; i < n; ++i) {
    saw0 |= c[i].first_w == 0 && c[i].last_w == 0;
    saw13 |= c[i].first_w == 13 && c[i].last_w == 13;
    saw_disjoint_truth |= c[i].first_w == 30 && c[i].last_w == 34;
    if (i < 2)
      saw_forced_span_first |= c[i].first_w == 0 && c[i].last_w == 13;
    for (int j = i + 1; j < n; ++j)
      EXPECT_FALSE(c[i].first_w == c[j].first_w && c[i].last_w == c[j].last_w);
  }
  EXPECT_TRUE(saw0);
  EXPECT_TRUE(saw13);
  EXPECT_TRUE(saw_disjoint_truth) << "oracle peaks must rank, never veto, the exhaustive fallback";
  EXPECT_FALSE(saw_forced_span_first) << "separate peaks must not be fused into one CORESET";
}

// ---- Technique D: does the search actually contain, and correctly realise, the truth? ---------
// Offline ground truth for THIS cell, from the gNB's own config/log:
//   pdsch TDA S=1 L=13 - dmrs additionalPosition 2, maxLength 1 - mcs_table qam256
//   dl_dmrs_symb_pos = 0x884  (symbols 2, 7, 11)

TEST(TechniqueD, HypothesisSetContainsThisCellsTruth) {
  // If the truth is not enumerated, the sweep can never converge however long it runs -- and it
  // would report "undecided" forever rather than failing loudly.
  nr_pdsch_config_sweep_state_t st;
  const int n = nr_pdsch_config_sweep_init(&st, 2);
  bool found = false;
  for (int i = 0; i < n; i++) {
    if (st.hyp[i].tda_start == 1 && st.hyp[i].tda_length == 13 && st.hyp[i].dmrs_add_pos == 2
        && st.hyp[i].dmrs_max_len == 1 && st.hyp[i].mcs_table == 1) {
      found = true;
    }
  }
  EXPECT_TRUE(found) << "this deployment's real config is not among the swept hypotheses";
}

TEST(TechniqueD, TruthHypothesisRealisesTheGnbsOwnDmrsMask) {
  // The sweep only helps if applying a hypothesis reproduces the real PDU. dmrs_TypeA_Position is
  // the ASN.1 ENUM (pos2 = 0), the trap that cost a whole earlier investigation.
  const int32_t mask = nr_pdcch_blind_dmrs_mask(0 /* pos2 */, 13 /* L */, 1 /* S */,
                                                0 /* mapping type A */, 2 /* add_pos */, 1 /* len */);
  EXPECT_EQ(mask, 0x884) << "hypothesis does not reproduce the gNB's dl_dmrs_symb_pos";
}

TEST(TechniqueD, AWrongHypothesisDoesNotReproduceTheRealMask) {
  // Sanity on the discriminator itself: if every hypothesis produced the same mask, the TB-CRC
  // oracle would have nothing to separate.
  const int32_t m_addpos = nr_pdcch_blind_dmrs_mask(0, 13, 1, 0, 0 /* add_pos 0 */, 1);
  EXPECT_NE(m_addpos, 0x884);
  const int32_t m_tda = nr_pdcch_blind_dmrs_mask(0, 7 /* L=7 */, 1, 0, 2, 1);
  EXPECT_NE(m_tda, 0x884);
}


TEST_F(BlindPdcchTest, UlAutoFlagSeparatesRawDiscoveryFromManualInterpretation) {
  auto configured=LiveUlOpts();
  UlGroundTruth gt;
  gt.riv=1200; gt.mcs=10; gt.ndi=1;
  const auto len=nr_pdcch_blind_dci01_size(&configured);
  auto llr=EncodeToLLR(PackUlPayload(gt,configured),gt.rnti,len,kAggregationLevel,40.0,rng_);
  nr_pdcch_blind_ul_result_t manual,auto_raw;
  ASSERT_TRUE(nr_pdcch_blind_decode_01_mode(false,llr.data(),kAggregationLevel,len,
                                          &configured,gt.rnti,gt.rnti,&manual));
  EXPECT_TRUE(manual.plausible);
  EXPECT_EQ(manual.width_hyp_class,-1);
  EXPECT_EQ(manual.interp_hyp_class,-1);
  EXPECT_EQ(manual.hyp_generation,0u);
  /* Unknown interpretation deliberately wrong: a real CRC still identifies the length. */
  configured.tda_count=16;
  configured.bwp_size=0;
  ASSERT_TRUE(nr_pdcch_blind_decode_01_mode(true,llr.data(),kAggregationLevel,len,
                                          &configured,gt.rnti,gt.rnti,&auto_raw));
  EXPECT_FALSE(auto_raw.plausible); // raw evidence is never a claimed usable grant
  EXPECT_EQ(auto_raw.raw_payload,manual.raw_payload);
  EXPECT_EQ(auto_raw.rnti,gt.rnti);
  EXPECT_FALSE(nr_pdcch_blind_decode_01_mode(false,llr.data(),kAggregationLevel,len,
                                           &configured,gt.rnti,gt.rnti,&manual));
}
TEST_F(BlindPdcchTest, UlRawOracleRejectsDlAndWrongRnti) {
  GroundTruth dl;
  auto dl_len=nr_pdcch_blind_dci_size(dl.bwp_size);
  auto dlllr=EncodeToLLR(PackPayload(dl,RivBitsFor(dl.bwp_size)),dl.rnti,dl_len,
                         kAggregationLevel,40.0,rng_);
  nr_pdcch_blind_ul_result_t out;
  EXPECT_FALSE(nr_pdcch_blind_decode_raw_01(dlllr.data(),kAggregationLevel,dl_len,dl.rnti,dl.rnti,&out));
  auto opts=LiveUlOpts();
  UlGroundTruth ul;
  auto len=nr_pdcch_blind_dci01_size(&opts);
  auto llr=EncodeToLLR(PackUlPayload(ul,opts),ul.rnti,len,kAggregationLevel,40.0,rng_);
  EXPECT_FALSE(nr_pdcch_blind_decode_raw_01(llr.data(),kAggregationLevel,len,ul.rnti+1,ul.rnti+1,&out));
  EXPECT_FALSE(nr_pdcch_blind_decode_raw_01(llr.data(),3,len,ul.rnti,ul.rnti,&out));
}


TEST_F(BlindPdcchTest, UlWidthEquivalenceUsesActualExtractedGrantsAcrossPayloads) {
  auto opts=LiveUlOpts();
  const uint16_t len=nr_pdcch_blind_dci01_size(&opts);
  std::vector<nr_hyp_t> raw(NR_HYP_SWEEP_MAX_RAW);
  const int count=nr_pdcch_ul_field_sweep_generate(&opts,len,raw.data(),raw.size());
  ASSERT_EQ(count,53);
  struct Context { nr_pdcch_blind_ul_opts_t opts; uint16_t len; } context{opts,len};
  std::vector<uint64_t> payloads;
  for(int i=0;i<8;++i) {
    UlGroundTruth gt; gt.riv=273*(i+1); gt.mcs=i+2; gt.harq_pid=i; gt.ndi=i%2;
    payloads.push_back(PackUlPayload(gt,opts));
  }
  const void *samples[8];
  for(int i=0;i<8;++i) samples[i]=&payloads[i];
  auto equivalent=[](const nr_hyp_t *a,const nr_hyp_t *b,const void *sample,void *opaque)->bool {
    auto *c=static_cast<Context*>(opaque);
    auto oa=c->opts,ob=c->opts;
    nr_pdcch_ul_field_sweep_apply(a,&oa); nr_pdcch_ul_field_sweep_apply(b,&ob);
    nr_pdcch_blind_ul_result_t ga,gb;
    uint64_t p=*static_cast<const uint64_t*>(sample);
    if(!nr_pdcch_blind_extract_01(p,c->len,0x1234,&oa,&ga) ||
       !nr_pdcch_blind_extract_01(p,c->len,0x1234,&ob,&gb)) return false;
    return !memcmp(&ga,&gb,sizeof(ga));
  };
  nr_hyp_sweep_state_t st;
  int classes=nr_hyp_sweep_init(&st,raw.data(),count,nullptr,nullptr,equivalent,samples,8,&context);
  EXPECT_TRUE(classes>0 || classes==NR_HYP_SWEEP_CLASS_OVERFLOW);
  if(classes>0) {
    for(int a=0;a<count;++a)
      for(int b=0;b<a;++b)
        if(st.class_of_raw[a]==st.class_of_raw[b])
          for(auto sample:samples) { EXPECT_TRUE(equivalent(&raw[a],&raw[b],sample,&context)); }
  } else { EXPECT_EQ(nr_hyp_sweep_winner(&st),-1); }
  printf("UL real-extractor equivalence: raw=%d classes_or_refusal=%d samples=8\n",count,classes);
}


TEST_F(BlindPdcchTest, UlControllerAttributesFeedbackAndRejectsPreviousGeneration) {
  auto opts=LiveUlOpts();
  uint16_t len=nr_pdcch_blind_dci01_size(&opts);
  auto prime=[&](nr_pdcch_blind_ul_result_t *out)->bool {
    bool got=false;
    for(int i=0;i<8;++i) {
      UlGroundTruth gt; gt.riv=273*(i+1); gt.mcs=i+2; gt.harq_pid=i; gt.ndi=i%2;
      got=nr_pdcch_ul_discovery_grant(&opts,len,gt.rnti,PackUlPayload(gt,opts),out);
    }
    return got;
  };
  nr_pdcch_ul_discovery_reset();
  nr_pdcch_blind_ul_result_t old_grant{},new_grant{};
  ASSERT_TRUE(prime(&old_grant));
  ASSERT_GE(old_grant.width_hyp_class,0);
  ASSERT_GT(old_grant.hyp_generation,0u);
  EXPECT_EQ(nr_pdcch_ul_discovery_snapshot().width_trials,0u);
  nr_pdcch_ul_discovery_feedback(&old_grant,true);
  EXPECT_EQ(nr_pdcch_ul_discovery_snapshot().width_trials,1u);
  nr_pdcch_ul_discovery_reset();
  ASSERT_TRUE(prime(&new_grant));
  EXPECT_NE(old_grant.hyp_generation,new_grant.hyp_generation);
  nr_pdcch_ul_discovery_feedback(&old_grant,true);
  EXPECT_EQ(nr_pdcch_ul_discovery_snapshot().width_trials,0u);
  nr_pdcch_ul_discovery_feedback(&new_grant,false);
  EXPECT_EQ(nr_pdcch_ul_discovery_snapshot().width_trials,1u);
  nr_pdcch_ul_discovery_reset();
}

int main(int argc, char** argv)
{
  crcTableInit(); // Match PHY initialization; synthetic encode/decode alone can hide zero tables.
  logInit();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

TEST_F(BlindPdcchTest, RawDlLengthRecoveryDoesNotRequireAnInterpretation) {
  for(int len : {37,47,55}) {
    const uint64_t payload=(UINT64_C(1)<<(len-1)) | UINT64_C(0x1a3b5c7d);
    auto llr=EncodeToLLR(payload,0x4b31,len,2,40.0,rng_);
    nr_pdcch_blind_raw_result_t raw{};
    ASSERT_TRUE(nr_pdcch_blind_decode_raw_11(llr.data(),2,len,0x4b31,0x4b31,&raw));
    EXPECT_EQ(raw.rnti,0x4b31);
    EXPECT_EQ(raw.payload,payload);
    EXPECT_EQ(raw.mismatched_bits,0);
    nr_pdcch_blind_extract_opts_t wrong=DefaultOpts();
    wrong.tda_count=16; wrong.tb2_bits=8;
    nr_pdcch_blind_result_t parsed{};
    EXPECT_FALSE(nr_pdcch_blind_decode_and_extract_ex(llr.data(),2,len,273,0,
                                                     0x4b31,0x4b31,&wrong,&parsed));
    nr_pdcch_blind_ul_result_t ul{};
    EXPECT_FALSE(nr_pdcch_blind_decode_raw_01(llr.data(),2,len,0x4b31,0x4b31,&ul));
  }
}
TEST_F(BlindPdcchTest, RawDlRejectsUlDirectionAndInvalidInputs) {
  auto llr=EncodeToLLR(UINT64_C(0x13579),0x4b31,47,2,40.0,rng_);
  nr_pdcch_blind_raw_result_t raw{};
  EXPECT_FALSE(nr_pdcch_blind_decode_raw_11(llr.data(),2,47,0x4b31,0x4b31,&raw));
  EXPECT_FALSE(nr_pdcch_blind_decode_raw_11(nullptr,2,47,1,65519,&raw));
  EXPECT_FALSE(nr_pdcch_blind_decode_raw_11(llr.data(),3,47,1,65519,&raw));
  EXPECT_FALSE(nr_pdcch_blind_decode_raw_11(llr.data(),2,64,1,65519,&raw));
  EXPECT_FALSE(nr_pdcch_blind_decode_raw_11(llr.data(),2,47,2,1,&raw));
}

/* CONTRACT CHANGED (superseding the 2026-09-09 note this replaces): nr_pdcch_ul_discovery.c's
 * context matching now includes the RNTI again, so evidence is no longer pooled across UEs that
 * merely share a DCI length -- see that file's own comment at the context-matching loop and at
 * decode_equivalent/same_options. Pooling required a separate "pooled winner corroborated across
 * every contributing identity" veto to avoid confidently imposing one UE's layout on another UE
 * that shares a length but not a configuration; that veto and its bookkeeping (contrib_rnti/
 * contrib_trials/contrib_passes) are gone from the source, not merely untested, so asserting a
 * shared generation here would be asserting a contract the implementation no longer provides.
 * Each identity now gets its own context, its own generation, and inherits nothing from another
 * RNTI at the same length -- re-tested below. The generation-tagging safety property the original
 * test protected (a reset invalidates queued feedback) is unchanged in substance and re-asserted
 * at the end, unchanged from before. */
TEST_F(BlindPdcchTest, UlControllerKeepsEachIdentityIndependentAtTheSameDciLength) {
  const auto opts=LiveUlOpts();
  const uint16_t len=nr_pdcch_blind_dci01_size(&opts);
  nr_pdcch_ul_discovery_reset();
  nr_pdcch_blind_ul_result_t grant[3]{};
  for(int i=0;i<8;++i) {
    for(int u=0;u<3;++u) {
      UlGroundTruth gt;
      gt.rnti=0x1234+u; gt.riv=273*(i+1); gt.mcs=i+2; gt.harq_pid=i; gt.ndi=i%2;
      bool got=nr_pdcch_ul_discovery_grant(&opts,len,gt.rnti,PackUlPayload(gt,opts),&grant[u]);
      if(i==7) { ASSERT_TRUE(got) << "UE " << u << " lost its accumulated samples"; }
    }
  }
  // Three identities, three independent contexts: no generation is shared with another.
  EXPECT_NE(grant[0].hyp_generation,grant[1].hyp_generation);
  EXPECT_NE(grant[1].hyp_generation,grant[2].hyp_generation);
  EXPECT_NE(grant[0].hyp_generation,grant[2].hyp_generation);
  // Feedback lands once per grant, arriving out of order, and all three count -- into three
  // separate contexts' trial totals, not one pooled total.
  for(int u : {2,0,1}) nr_pdcch_ul_discovery_feedback(&grant[u],u!=1);
  EXPECT_EQ(nr_pdcch_ul_discovery_snapshot().width_trials,3u);
  // The safety property that matters is unchanged: a reset invalidates queued feedback.
  nr_pdcch_ul_discovery_reset();
  for(auto &g:grant) nr_pdcch_ul_discovery_feedback(&g,true);
  EXPECT_EQ(nr_pdcch_ul_discovery_snapshot().width_trials,0u);
}

/* Pooling must NOT merge searches that are genuinely different problems. Two DCI lengths mean two
 * field layouts, so they must stay in separate contexts with separate generations -- otherwise one
 * UE's transport-block CRCs would score another's hypothesis set. */
TEST_F(BlindPdcchTest, UlControllerKeepsDifferentDciLengthsApart) {
  auto opts=LiveUlOpts();
  const uint16_t len_a=nr_pdcch_blind_dci01_size(&opts);
  nr_pdcch_ul_discovery_reset();
  nr_pdcch_blind_ul_result_t a{},b{};
  for(int i=0;i<8;++i) {
    UlGroundTruth gt; gt.rnti=0x3001; gt.riv=273*(i+1); gt.mcs=i+2; gt.harq_pid=i; gt.ndi=i%2;
    nr_pdcch_ul_discovery_grant(&opts,len_a,gt.rnti,PackUlPayload(gt,opts),&a);
  }
  for(int i=0;i<8;++i) {
    UlGroundTruth gt; gt.rnti=0x3002; gt.riv=273*(i+1); gt.mcs=i+2; gt.harq_pid=i; gt.ndi=i%2;
    nr_pdcch_ul_discovery_grant(&opts,(uint16_t)(len_a+1),gt.rnti,PackUlPayload(gt,opts),&b);
  }
  EXPECT_NE(a.hyp_generation,b.hyp_generation);
  EXPECT_GE(nr_pdcch_ul_discovery_snapshot().raw_samples,16);
  nr_pdcch_ul_discovery_reset();
}

/* Eviction still discards ONLY the evicted context's evidence. Contexts are now filled by distinct
 * DCI lengths rather than distinct RNTIs, which is what the pooling change made them mean. */
TEST_F(BlindPdcchTest, UlControllerEvictionDiscardsOnlyEvictedContextFeedback) {
  auto opts=LiveUlOpts();
  const uint16_t base=nr_pdcch_blind_dci01_size(&opts);
  nr_pdcch_ul_discovery_reset();
  nr_pdcch_blind_ul_result_t first{},last{};
  for(int c=0;c<=NR_PDCCH_BLIND_MAX_UE;++c) {
    for(int i=0;i<8;++i) {
      UlGroundTruth gt;
      gt.rnti=(uint16_t)(0x2000+c); gt.riv=273*(i+1); gt.mcs=i+2; gt.harq_pid=i; gt.ndi=i%2;
      nr_pdcch_ul_discovery_grant(&opts,(uint16_t)(base+c),gt.rnti,PackUlPayload(gt,opts),&last);
    }
    if(c==0) first=last;
  }
  nr_pdcch_ul_discovery_feedback(&first,true);   // evicted context: must be ignored
  const auto after_first=nr_pdcch_ul_discovery_snapshot().width_trials;
  nr_pdcch_ul_discovery_feedback(&last,true);    // live context: must count
  EXPECT_GT(nr_pdcch_ul_discovery_snapshot().width_trials,after_first);
  nr_pdcch_ul_discovery_reset();
}

#include "executables/nr_rx_continuity.h"
TEST(RxContinuity, ReacquisitionDoesNotCauseAnInfiniteFalseGapLoop) {
  nr_rx_continuity_t s{};
  EXPECT_TRUE(nr_rx_continuity_check(&s,1000));
  nr_rx_continuity_commit(&s,1000,100);
  EXPECT_TRUE(nr_rx_continuity_check(&s,1100));
  EXPECT_FALSE(nr_rx_continuity_check(&s,1150)); // genuine unannounced loss
  nr_rx_continuity_reset(&s); // actual receiver does this before acquisition
  EXPECT_TRUE(nr_rx_continuity_check(&s,1000000)); // acquisition's discarded frames
  nr_rx_continuity_commit(&s,1000000,100);
  EXPECT_TRUE(nr_rx_continuity_check(&s,1000100));
  EXPECT_FALSE(nr_rx_continuity_check(&s,1000101)); // reset must not hide later loss
}
TEST(RxContinuity, PrefixReadsAndExplicitRebasesHaveDifferentContracts) {
  nr_rx_continuity_t s{};
  nr_rx_continuity_commit(&s,5000,100+8);
  EXPECT_FALSE(nr_rx_continuity_check(&s,5100));
  EXPECT_TRUE(nr_rx_continuity_check(&s,5108));
  nr_rx_continuity_reset(&s);
  EXPECT_TRUE(nr_rx_continuity_check(&s,5300));
  nr_rx_continuity_commit(&s,5300,0);
  EXPECT_FALSE(s.valid);
}

TEST_F(BlindPdcchTest, DlLayoutFamilyRecoversVaryingFrontWidthsWithoutManualHints) {
  for(int bw=0;bw<=2;++bw) for(int td=0;td<=4;++td) {
    auto opts=DefaultOpts();
    opts.bwp_indicator_bits=bw;
    opts.tda_count=1<<td;
    for(int i=0;i<opts.tda_count;++i) { opts.tda_start[i]=1; opts.tda_length[i]=13; }
    GroundTruth gt;
    gt.bwp_size=273; gt.riv=273*20+7; gt.mcs=13; gt.rv=0; gt.ndi=1;
    gt.harq_pid=9; gt.time_domain_assignment=opts.tda_count-1;
    const uint16_t len=nr_pdcch_blind_dci_size_ex(273,&opts);
    const auto payload=PackPayload(gt,16,&opts);
    auto llr=EncodeToLLR(payload,gt.rnti,len,2,40.0,rng_);
    nr_pdcch_blind_raw_result_t raw{};
    ASSERT_TRUE(nr_pdcch_blind_decode_raw_11(llr.data(),2,len,gt.rnti,gt.rnti,&raw));
    nr_pdcch_blind_result_t candidates[3]{};
    uint8_t ids[3]{};
    const int n=nr_pdcch_blind_dl_layout_candidates(&raw,len,273,0,candidates,ids);
    bool found=false;
    for(int i=0;i<n;++i) if(ids[i]==bw*5+td) {
      found=true;
      EXPECT_EQ(candidates[i].start_rb,7);
      EXPECT_EQ(candidates[i].num_rb,21);
      EXPECT_EQ(candidates[i].mcs,13);
      EXPECT_EQ(candidates[i].harq_pid,9);
      EXPECT_EQ(candidates[i].tda_index,opts.tda_count-1);
    }
    EXPECT_TRUE(found) << "widths " << bw << "/" << td;
  }
}
TEST_F(BlindPdcchTest, DlLayoutCandidatesDoNotDeclareAUniqueConfiguration) {
  nr_pdcch_blind_raw_result_t raw{};
  raw.rnti=0x1234;
  raw.payload=UINT64_C(1)<<46; // 47-bit payload with ambiguous all-zero fields
  nr_pdcch_blind_result_t candidates[3]{};
  uint8_t ids[3]{};
  EXPECT_EQ(nr_pdcch_blind_dl_layout_candidates(&raw,47,273,0,candidates,ids),2);
  EXPECT_EQ(nr_pdcch_blind_dl_layout_candidates(&raw,30,273,0,candidates,ids),0);
  EXPECT_EQ(nr_pdcch_blind_dl_layout_candidates(nullptr,47,273,0,candidates,ids),0);
}

TEST_F(BlindPdcchTest, PolarCleanupCannotFreeAnInFlightDecoderCacheEntry) {
  t_nrPolar_params *held=nr_polar_params(NR_POLAR_DCI_MESSAGE_TYPE,47,2);
  ASSERT_NE(held,nullptr);
  ASSERT_FALSE(nr_polar_try_cleanup());
  EXPECT_EQ(held->payloadBits,47);
  EXPECT_EQ(held->n_pc_wm,0);
  polarReturn(held);
  EXPECT_TRUE(nr_polar_try_cleanup());
  // Re-initialize and exercise the actual codec after a clean cache release.
  const uint64_t p=(UINT64_C(1)<<46)|12345;
  auto llr=EncodeToLLR(p,0x3210,47,2,40.0,rng_);
  nr_pdcch_blind_raw_result_t raw{};
  ASSERT_TRUE(nr_pdcch_blind_decode_raw_11(llr.data(),2,47,0x3210,0x3210,&raw));
  EXPECT_EQ(raw.payload,p);
}

TEST(OverTheAirCommonConfig, KeepsCommonFactsSeparateFromDedicatedHypotheses) {
  nr_pdcch_blind_reset_common();
  nr_pdcch_blind_common_config_t f{},got{};
  f.pci=42; f.dl_bwp_size=106; f.ul_bwp_start=2; f.ul_bwp_size=100;
  f.dl_mu=f.ul_mu=1; f.dl_count=1; f.ul_count=2;
  f.dl_start[0]=2; f.dl_length[0]=12;
  f.ul_start[0]=0; f.ul_length[0]=14; f.ul_k2[0]=3;
  f.ul_start[1]=4; f.ul_length[1]=10; f.ul_mapping[1]=1; f.ul_k2[1]=6;
  const auto before=*nr_pdcch_blind_monitor_get_cfg();
  ASSERT_TRUE(nr_pdcch_blind_publish_common(&f));
  ASSERT_TRUE(nr_pdcch_blind_get_common(42,&got));
  EXPECT_EQ(memcmp(&f,&got,sizeof(f)),0);
  EXPECT_EQ(memcmp(&before,nr_pdcch_blind_monitor_get_cfg(),sizeof(before)),0)
      << "SIB1 common config must not overwrite dedicated assumptions";
  EXPECT_FALSE(nr_pdcch_blind_get_common(43,&got));
  EXPECT_EQ(got.ul_bwp_size,0);
  f.ul_length[1]=11; // 4+11 exceeds a slot
  EXPECT_FALSE(nr_pdcch_blind_publish_common(&f));
  nr_pdcch_blind_reset_common();
  EXPECT_FALSE(nr_pdcch_blind_get_common(42,&got));
}

TEST_F(BlindPdcchTest, UlDefaultK2AndDmrsUseMeasuredCellParameters) {
  auto opts=LiveUlOpts();
  opts.tda_count=0; opts.numerology=2; opts.dmrs_typeA_position=1;
  opts.phy_cell_id=317;
  UlGroundTruth gt; gt.rnti=0x721;gt.tda_index=0;gt.riv=273*7;
  nr_pdcch_blind_ul_result_t out{};
  const auto len=nr_pdcch_blind_dci01_size(&opts);
  ASSERT_TRUE(nr_pdcch_blind_extract_01(PackUlPayload(gt,opts),len,gt.rnti,&opts,&out));
  EXPECT_EQ(out.k2,2); // default table first row: j; mu=2 => j=2
  EXPECT_NE(out.ul_dmrs_symb_pos & (1<<3),0);
  EXPECT_EQ(out.ul_dmrs_symb_pos & (1<<2),0);
  EXPECT_EQ(out.data_scrambling_id,317);
  EXPECT_EQ(out.ul_dmrs_scrambling_id,317);
  opts.numerology=3;
  ASSERT_TRUE(nr_pdcch_blind_extract_01(PackUlPayload(gt,opts),len,gt.rnti,&opts,&out));
  EXPECT_EQ(out.k2,3);
  opts.numerology=6;
  EXPECT_FALSE(nr_pdcch_blind_extract_01(PackUlPayload(gt,opts),len,gt.rnti,&opts,&out));
}

TEST_F(BlindPdcchTest, UlRawEvidenceSurvivesUnknownInterpretation) {
  nr_pdcch_ul_discovery_reset();
  auto packing=LiveUlOpts();
  auto unknown=packing;unknown.bwp_size=0;unknown.tda_count=0;
  const auto len=nr_pdcch_blind_dci01_size(&packing);
  nr_pdcch_blind_ul_result_t result{};
  for(int i=0;i<8;i++) {
    UlGroundTruth gt;gt.riv=273*(i+1);gt.mcs=i+2;
    EXPECT_FALSE(nr_pdcch_ul_discovery_grant(&unknown,len,gt.rnti,PackUlPayload(gt,packing),&result));
  }
  const auto snapshot=nr_pdcch_ul_discovery_snapshot();
  EXPECT_EQ(snapshot.raw_samples,8);
  EXPECT_EQ(snapshot.width_trials,0u);
  EXPECT_EQ(snapshot.interp_trials,0u);
  nr_pdcch_ul_discovery_reset();
}

TEST_F(BlindPdcchTest, UlDmrsMaskLookupRejectsInvalidGeometryWithoutAborting) {
  EXPECT_EQ(nr_pdcch_blind_ul_dmrs_mask(14,0,0,2,1,0),0x884);
  EXPECT_EQ(nr_pdcch_blind_ul_dmrs_mask(10,4,0,2,1,0),-1);
  EXPECT_EQ(nr_pdcch_blind_ul_dmrs_mask(0,0,0,2,1,0),-1);
  EXPECT_EQ(nr_pdcch_blind_ul_dmrs_mask(14,255,0,2,1,0),-1);
  EXPECT_EQ(nr_pdcch_blind_ul_dmrs_mask(14,0,2,2,1,0),-1);
  EXPECT_EQ(nr_pdcch_blind_ul_dmrs_mask(14,0,0,2,3,0),-1);
  EXPECT_EQ(nr_pdcch_blind_ul_dmrs_mask(14,0,0,2,1,2),-1);
}

/* Gap: dmrs_max_length was pruned to <=1 everywhere in nr_pdcch_ul_discovery.c ("double-symbol
 * front-loading is receiver scope this monitor cannot resolve"), even though
 * nr_pdcch_blind_ul_dmrs_mask() already carries the maxLength-2 table (g_table_6_4_1_1_3_4) and
 * blind_ul_finish() already threads opts->dmrs_max_length through to it unconditionally -- the
 * mask lookup itself needed no change, only removing the upstream pruning that never let a
 * maxLength-2 hypothesis reach it. TS 38.211 Table 6.4.1.1.3-4 row ld=14 col add_pos=1 = 3072;
 * verified against the in-tree array, not derived independently, same practice as the sibling
 * UlDmrsMaskLookupRejectsInvalidGeometryWithoutAborting test above. */
TEST_F(BlindPdcchTest, UlDmrsMaskLookupSupportsMaxLength2) {
  // add_pos 0/1 (mapping type A) are the only ones TS 38.211 Table 6.4.1.1.3-4 defines.
  EXPECT_EQ(nr_pdcch_blind_ul_dmrs_mask(14, 0, 0, 1, 2, 0), 0xC0C);
  // add_pos 2/3 are reserved at maxLength 2 (columns 2/3 are -1 in every row of that table) -- must
  // be REJECTED, not silently misread as some other position.
  EXPECT_EQ(nr_pdcch_blind_ul_dmrs_mask(14, 0, 0, 2, 2, 0), -1);
  EXPECT_EQ(nr_pdcch_blind_ul_dmrs_mask(14, 0, 0, 3, 2, 0), -1);
}

/* End-to-end: a maxLength-2 hypothesis must now actually reach nr_pdcch_blind_extract_01() and
 * decode to a symbol mask with MORE DM-RS symbols than the maxLength-1 case -- not just avoid being
 * pruned, but genuinely engage the double-symbol table. */
TEST_F(BlindPdcchTest, UlAntennaPortsAcceptsMaxLength2AndDoublesTheDmrsSymbolCount) {
  auto opts1 = LiveUlOpts();
  opts1.dmrs_max_length = 1;
  opts1.dmrs_add_pos = 1;
  UlGroundTruth gt1;
  gt1.riv = PRBalloc_to_locationandbandwidth0(24, 8, 273);
  gt1.mcs = 10;
  gt1.antenna_ports = 2;
  const auto len1 = nr_pdcch_blind_dci01_size(&opts1);
  nr_pdcch_blind_ul_result_t out1{};
  ASSERT_TRUE(nr_pdcch_blind_extract_01(PackUlPayload(gt1, opts1), len1, gt1.rnti, &opts1, &out1));

  auto opts2 = LiveUlOpts();
  opts2.dmrs_max_length = 2;
  opts2.dmrs_add_pos = 1;
  UlGroundTruth gt2;
  gt2.riv = PRBalloc_to_locationandbandwidth0(24, 8, 273);
  gt2.mcs = 10;
  gt2.antenna_ports = 2;
  const auto len2 = nr_pdcch_blind_dci01_size(&opts2);
  nr_pdcch_blind_ul_result_t out2{};
  ASSERT_TRUE(nr_pdcch_blind_extract_01(PackUlPayload(gt2, opts2), len2, gt2.rnti, &opts2, &out2))
      << (out2.reject_reason ? out2.reject_reason : "-");
  EXPECT_GT(__builtin_popcount((unsigned)out2.ul_dmrs_symb_pos), __builtin_popcount((unsigned)out1.ul_dmrs_symb_pos));

  // The reserved add_pos/maxLength-2 combination must still be rejected end to end, not just at
  // the raw table-lookup level.
  auto opts3 = LiveUlOpts();
  opts3.dmrs_max_length = 2;
  opts3.dmrs_add_pos = 2;
  UlGroundTruth gt3 = gt2;
  const auto len3 = nr_pdcch_blind_dci01_size(&opts3);
  nr_pdcch_blind_ul_result_t out3{};
  EXPECT_FALSE(nr_pdcch_blind_extract_01(PackUlPayload(gt3, opts3), len3, gt3.rnti, &opts3, &out3));
  ASSERT_NE(out3.reject_reason, nullptr);
  EXPECT_STREQ(out3.reject_reason, "no valid PUSCH DM-RS position for this allocation length");
}

TEST_F(BlindPdcchTest, UlMcsBoundaryUsesActualSelectedTable) {
  for(int table=0;table<3;++table) {
    auto opts=LiveUlOpts(); opts.mcs_table=table;
    UlGroundTruth gt; gt.mcs=28; gt.riv=273;
    nr_pdcch_blind_ul_result_t result{};
    bool ok=nr_pdcch_blind_extract_01(PackUlPayload(gt,opts),nr_pdcch_blind_dci01_size(&opts),gt.rnti,&opts,&result);
    EXPECT_EQ(ok,table!=1) << table;
    EXPECT_EQ(nr_get_code_rate_ul(28,table)>0,table!=1);
    gt.mcs=29;
    EXPECT_FALSE(nr_pdcch_blind_extract_01(PackUlPayload(gt,opts),nr_pdcch_blind_dci01_size(&opts),gt.rnti,&opts,&result));
  }
}
TEST_F(BlindPdcchTest, DlMcs28SurvivesUntilTableInterpretation) {
  GroundTruth gt; gt.bwp_size=106; gt.riv=PRBalloc_to_locationandbandwidth0(24,8,106);
  gt.time_domain_assignment=1; gt.mcs=28;
  auto len=nr_pdcch_blind_dci_size(gt.bwp_size);
  auto llr=EncodeToLLR(PackPayload(gt,RivBitsFor(gt.bwp_size)),gt.rnti,len,kAggregationLevel,40.0,rng_);
  nr_pdcch_blind_result_t result{};
  EXPECT_TRUE(nr_pdcch_blind_decode_and_extract(llr.data(),kAggregationLevel,len,gt.bwp_size,
      kDmrsTypeAPositionPos2,NR_PDCCH_BLIND_RNTI_MIN_DEFAULT,NR_PDCCH_BLIND_RNTI_MAX_DEFAULT,&result));
  EXPECT_EQ(result.mcs,28);
}

/* tda_count == 0 is the TS 38.214 default 16-entry table -- a COMPLETE interpretation, not an
 * unknown. It used to be refused alongside a missing BWP, which is why autonomous UL never made a
 * single attempt. A UL BWP is still genuinely required (it is the RIV reference). */
TEST_F(BlindPdcchTest, UlDefaultTdaTableArmsTheWidthSearch) {
  nr_pdcch_ul_discovery_reset();
  auto packing = LiveUlOpts();
  packing.tda_count = 0;                       // default table => 4-bit TDA field
  auto seeded = packing;                       // BWP known (SIB1-seeded at runtime), TDA defaulted
  const auto len = nr_pdcch_blind_dci01_size(&packing);
  nr_pdcch_blind_ul_result_t result{};
  for (int i = 0; i < 8; i++) {
    UlGroundTruth gt; gt.riv = 273 * (i + 1); gt.mcs = i + 2;
    nr_pdcch_ul_discovery_grant(&seeded, len, gt.rnti, PackUlPayload(gt, packing), &result);
  }
  const auto armed = nr_pdcch_ul_discovery_snapshot();
  EXPECT_EQ(armed.raw_samples, 8);
  EXPECT_GT(armed.width_classes, 0) << "default-TDA search refused to arm";

  // ... and a missing UL BWP must still refuse: it is the RIV reference, not a defaultable field.
  nr_pdcch_ul_discovery_reset();
  auto no_bwp = packing; no_bwp.bwp_size = 0;
  for (int i = 0; i < 8; i++) {
    UlGroundTruth gt; gt.riv = 273 * (i + 1); gt.mcs = i + 2;
    EXPECT_FALSE(nr_pdcch_ul_discovery_grant(&no_bwp, len, gt.rnti, PackUlPayload(gt, packing), &result));
  }
  EXPECT_EQ(nr_pdcch_ul_discovery_snapshot().width_classes, 0);
  nr_pdcch_ul_discovery_reset();
}

/* DCI 0_0 is size-aligned with 1_0 (TS 38.212 7.3.1.0), so the SAME polar decode yields both. The
 * 1_0 path rejects identifier=0 -- and those rejects are UL grants. This pins the contract the RT
 * path now relies on: the rejected 1_0 result still carries its decoded payload, and that payload
 * re-reads as a valid 0_0 grant with no second decode and no field-width hypothesis. */
TEST_F(BlindPdcchTest, Dci00RidesTheSame10Decode) {
  const uint16_t bwp = 273;
  const int riv_bits = RivBitsFor(bwp);
  const uint16_t len = nr_pdcch_blind_dci10_size(bwp);
  ASSERT_EQ(len, 28 + riv_bits);

  auto ul = LiveUlOpts();                       // bwp_size 273, tda_count 2, entry 0 = S:0 L:14 k2:4
  const uint32_t riv = PRBalloc_to_locationandbandwidth0(24, 8, bwp);
  const uint16_t rnti = 0x4656;

  // Pack a real format-0_0 payload, MSB-first, with the size-alignment padding left at zero.
  uint64_t payload = 0; int pos = len;
  auto put = [&](uint64_t v, int w) { pos -= w; payload |= (v & ((1ULL << w) - 1ULL)) << pos; };
  put(0, 1);            // identifier: 0 = UL
  put(riv, riv_bits);
  put(0, 4);            // TDA index 0
  put(0, 1);            // no frequency hopping
  put(10, 5);           // MCS 10
  put(1, 1);            // NDI
  put(0, 2);            // RV 0
  put(3, 4);            // HARQ pid
  put(1, 2);            // TPC
  ASSERT_EQ(pos, 8) << "0_0 should leave exactly the 1_0 - 0_0 size difference as padding";

  auto llr = EncodeToLLR(payload, rnti, len, kAggregationLevel, 40.0, rng_);
  auto ctx = Dci10Ctx(NR_BLIND_SS_UE_SPECIFIC, bwp);
  nr_pdcch_blind_result_t dl{};
  EXPECT_FALSE(nr_pdcch_blind_decode_and_extract_10(llr.data(), kAggregationLevel, len, &ctx,
                                                    0x0001, 0xFFEF, nullptr, &dl));
  EXPECT_EQ(dl.rnti, rnti);
  ASSERT_EQ(dl.payload, payload) << "the rejected 1_0 result must still expose its decoded payload";

  nr_pdcch_blind_ul_result_t ul00{};
  ASSERT_TRUE(nr_pdcch_blind_extract_00(dl.payload, len, dl.rnti, &ul, &ul00))
      << (ul00.reject_reason ? ul00.reject_reason : "-");
  EXPECT_EQ(ul00.rnti, rnti);
  EXPECT_EQ(ul00.start_rb, 8);
  EXPECT_EQ(ul00.num_rb, 24);
  EXPECT_EQ(ul00.mcs, 10);
  EXPECT_EQ(ul00.harq_pid, 3);
  EXPECT_EQ(ul00.k2, 4);
  EXPECT_EQ(ul00.ul_dci_format, NR_BLIND_UL_DCI_FORMAT_0_0);

  // A genuine DL 1_0 must NOT be mis-read as a UL grant: extract_00 re-checks the identifier.
  uint64_t dl_payload = payload | (1ULL << (len - 1));
  nr_pdcch_blind_ul_result_t not_ul{};
  EXPECT_FALSE(nr_pdcch_blind_extract_00(dl_payload, len, rnti, &ul, &not_ul));
}

/* A blind width hypothesis feeds RAW field values into the antenna-ports decode, which must REJECT
 * any code point outside its table rather than reach get_dmrs_port(), whose AssertFatal killed the
 * softmodem mid-capture (measured 2026-09-09, antenna_ports=14 under the OLD 4-row-only closed form
 * -> port bitmap 1<<12 -> "No dmrs port corresponding to layer 0 found"). Every in-domain code point
 * must still decode, and every out-of-domain one must still be rejected -- now cross-checked against
 * decode_dci_antenna_ports_val() directly (the same reverse table an attached UE's own PUSCH config
 * uses, TS 38.212 Table 7.3.1.1.2-8/9 for rank 1 / DM-RS type 1 / transform precoding disabled),
 * rather than a fixed "<=3" boundary: that boundary was an artifact of the old closed form's own
 * self-imposed 4-row scope, not a real spec limit -- the reverse table has 14 valid rows at rank 1
 * (rows 4-13 are the maxLength-2 rows a wider antenna_ports field can carry). */
TEST_F(BlindPdcchTest, UlAntennaPortsType1DecodesViaReverseTable) {
  auto opts = LiveUlOpts();
  opts.antenna_ports_bits = 5;                 // the sweep really does try 5 bits
  const auto len = nr_pdcch_blind_dci01_size(&opts);
  for (uint32_t ap = 0; ap < 32; ++ap) {
    UlGroundTruth gt;
    gt.riv = PRBalloc_to_locationandbandwidth0(24, 8, 273);
    gt.mcs = 10;
    gt.antenna_ports = ap;
    nr_pdcch_blind_ul_result_t out{};
    const bool ok = nr_pdcch_blind_extract_01(PackUlPayload(gt, opts), len, gt.rnti, &opts, &out);

    uint8_t exp_cdm = 0;
    uint16_t exp_ports = 0;
    int exp_fl = 0;
    const bool table_ok = decode_dci_antenna_ports_val(1, nullptr, NR_PUSCH_Config__transformPrecoder_disabled,
                                                       (uint8_t)ap, &exp_cdm, &exp_ports, &exp_fl) == 0;
    EXPECT_EQ(ok, table_ok) << "antenna_ports=" << ap << ": "
                            << (out.reject_reason ? out.reject_reason : "-");
    if (ok && table_ok) {
      EXPECT_EQ(out.n_dmrs_cdm_groups, exp_cdm) << "antenna_ports=" << ap;
      EXPECT_EQ(out.dmrs_ports, exp_ports) << "antenna_ports=" << ap;
      // Whatever it resolves to must name a port get_dmrs_port() can actually find.
      int low = 0;
      for (int i = 0; i < 12; i++) if ((out.dmrs_ports >> i) & 1) low++;
      EXPECT_GE(low, out.nrOfLayers) << "antenna_ports=" << ap << " left layer 0 without a port";
    }
  }
}

/* Same table, DM-RS type 2 (opts.dmrs_config_type = 1). Before this fix, EVERY type-2 hypothesis was
 * pruned upstream in nr_pdcch_ul_discovery.c's supported() gate and, even if it had reached here,
 * this function's closed form rejected every antenna_ports value above 3 unconditionally -- so a
 * type-2 UL cell produced zero UL grants regardless of aggregation level or RNTI range. */
TEST_F(BlindPdcchTest, UlAntennaPortsType2DecodesViaReverseTable) {
  auto opts = LiveUlOpts();
  opts.dmrs_config_type = 1;   // DM-RS type 2
  opts.antenna_ports_bits = 5; // type-2 rank-1 (Table 7.3.1.1.2-10/11) needs up to 28 code points
  const auto len = nr_pdcch_blind_dci01_size(&opts);
  long type2_tag = 1;
  for (uint32_t ap = 0; ap < 32; ++ap) {
    UlGroundTruth gt;
    gt.riv = PRBalloc_to_locationandbandwidth0(24, 8, 273);
    gt.mcs = 10;
    gt.antenna_ports = ap;
    nr_pdcch_blind_ul_result_t out{};
    const bool ok = nr_pdcch_blind_extract_01(PackUlPayload(gt, opts), len, gt.rnti, &opts, &out);

    uint8_t exp_cdm = 0;
    uint16_t exp_ports = 0;
    int exp_fl = 0;
    const bool table_ok = decode_dci_antenna_ports_val(1, &type2_tag, NR_PUSCH_Config__transformPrecoder_disabled,
                                                       (uint8_t)ap, &exp_cdm, &exp_ports, &exp_fl) == 0;
    EXPECT_EQ(ok, table_ok) << "antenna_ports=" << ap << ": "
                            << (out.reject_reason ? out.reject_reason : "-");
    if (ok && table_ok) {
      EXPECT_EQ(out.dmrs_config_type, 1);
      EXPECT_EQ(out.n_dmrs_cdm_groups, exp_cdm) << "antenna_ports=" << ap;
      EXPECT_EQ(out.dmrs_ports, exp_ports) << "antenna_ports=" << ap;
    }
  }
}

/* Transform precoding (DFT-s-OFDM): the antenna-ports field always uses lut_tp_rev (TS 38.212
 * Table 7.3.1.1.2-6/-7) regardless of the DM-RS-type hypothesis -- transform precoding mandates
 * DM-RS type 1 by spec. Before this fix, blind_ul_finish() used a hand-rolled closed form here
 * (cdm_groups=2; ports=1u<<antenna_ports) that was unreachable anyway (nr_pdcch_ul_discovery.c
 * refused every transform_precoding=1 hypothesis before it could reach this code), AND wrong for
 * antenna_ports>=4 (front_load 2 rows): lut_tp_rev wraps the port index there, the old formula
 * kept shifting it. */
TEST_F(BlindPdcchTest, UlAntennaPortsTransformPrecodingDecodesViaReverseTable) {
  auto opts = LiveUlOpts();
  opts.transform_precoding = 1;
  opts.antenna_ports_bits = 4; // lut_tp_rev has 12 rows, needs 4 bits
  const auto len = nr_pdcch_blind_dci01_size(&opts);
  for (uint32_t ap = 0; ap < 16; ++ap) {
    UlGroundTruth gt;
    gt.riv = PRBalloc_to_locationandbandwidth0(24, 8, 273);
    gt.mcs = 10;
    gt.antenna_ports = ap;
    nr_pdcch_blind_ul_result_t out{};
    const bool ok = nr_pdcch_blind_extract_01(PackUlPayload(gt, opts), len, gt.rnti, &opts, &out);

    uint8_t exp_cdm = 0;
    uint16_t exp_ports = 0;
    int exp_fl = 0;
    const bool table_ok = decode_dci_antenna_ports_val(1, nullptr, NR_PUSCH_Config__transformPrecoder_enabled,
                                                       (uint8_t)ap, &exp_cdm, &exp_ports, &exp_fl) == 0;
    EXPECT_EQ(ok, table_ok) << "antenna_ports=" << ap << ": "
                            << (out.reject_reason ? out.reject_reason : "-");
    if (ok && table_ok) {
      EXPECT_EQ(out.n_dmrs_cdm_groups, exp_cdm) << "antenna_ports=" << ap;
      EXPECT_EQ(out.dmrs_ports, exp_ports) << "antenna_ports=" << ap;
    }
  }
}

/* TS 38.214 6.1.4.1: transform-precoding-enabled UL uses Table 6.1.4.1-1 (opts.mcs_table=3, the
 * default-RRC-config case) or -2 (opts.mcs_table=4, qam64LowSE), never the non-TP tables 0-2 --
 * already implemented as nr_mac_common.c's Table_61411/Table_61412 (table_idx 3/4), just never
 * reachable from the passive discovery path before this fix (the interp-sweep generator produced
 * mcs_table in {0,1,2} for every hypothesis regardless of transform_precoding, so a TP-enabled
 * cell's true table could never be represented as a hypothesis at all). */
TEST_F(BlindPdcchTest, UlTransformPrecodingUsesTheTpMcsTables) {
  for (int table : {3, 4}) {
    auto opts = LiveUlOpts();
    opts.transform_precoding = 1;
    opts.mcs_table = table;
    UlGroundTruth gt;
    gt.riv = PRBalloc_to_locationandbandwidth0(24, 8, 273);
    gt.mcs = 10;
    const auto len = nr_pdcch_blind_dci01_size(&opts);
    nr_pdcch_blind_ul_result_t out{};
    ASSERT_TRUE(nr_pdcch_blind_extract_01(PackUlPayload(gt, opts), len, gt.rnti, &opts, &out))
        << "table=" << table << ": " << (out.reject_reason ? out.reject_reason : "-");
    EXPECT_EQ(out.mcs_table, table);

    // A reserved codepoint in THIS table must still be rejected -- proof the check actually reads
    // the TP table (found by walking down from the top of the MCS range) rather than always
    // passing via some non-TP fallback.
    uint32_t reserved_mcs = 31;
    while (reserved_mcs > 0 && nr_get_code_rate_ul((uint8_t)reserved_mcs, (uint8_t)table) != 0)
      --reserved_mcs;
    ASSERT_GT(reserved_mcs, 0u) << "table=" << table << " has no reserved MCS to test with";
    UlGroundTruth bad = gt;
    bad.mcs = reserved_mcs;
    nr_pdcch_blind_ul_result_t bad_out{};
    EXPECT_FALSE(nr_pdcch_blind_extract_01(PackUlPayload(bad, opts), len, bad.rnti, &opts, &bad_out))
        << "table=" << table << " mcs=" << reserved_mcs;
  }
}

TEST_F(BlindPdcchTest, AutoDci10RetainsRaTcAmbiguityAndNeverExportsAGrant) {
  // One real polar codeword, two different TS 38.212 7.3.1.2.1 field lists:
  // C/TC identifier=1,RIV=0; RA RIV=1024. Remaining fields are zero.
  constexpr uint16_t bwp = 48, rnti = 0x11;
  const uint16_t len = nr_pdcch_blind_dci10_size(bwp);
  Dci10Gt gt;
  const uint64_t payload = PackDci10Crnti(gt, RivBitsFor(bwp));
  auto llr = EncodeToLLR(payload, rnti, len, kAggregationLevel, 40.0, rng_);
  auto ctx = Dci10Ctx(NR_BLIND_SS_COMMON, bwp);
  auto opts = OptsWithTdaLists();
  nr_pdcch_blind_result_t out{}, manual{};
  nr_dci10_interpretation_report_t report{};
  ASSERT_TRUE(nr_pdcch_blind_decode_10_mode(false, llr.data(), kAggregationLevel,
      len, &ctx, 1, 0xffef, &opts, &manual, nullptr));
  EXPECT_EQ(manual.rnti_class, NR_BLIND_RNTI_CLASS_RA);
  EXPECT_FALSE(nr_pdcch_blind_decode_10_mode(true, llr.data(), kAggregationLevel,
      len, &ctx, 1, 0xffef, &opts, &out, &report));
  EXPECT_EQ(report.state, NR_DCI_AMBIGUOUS);
  ASSERT_EQ(report.attempted, 2u);
  ASSERT_EQ(report.surviving, 2u);
  EXPECT_EQ(report.unique_candidate, -1);
  EXPECT_EQ(report.candidates[0].rnti_class, NR_BLIND_RNTI_CLASS_RA);
  EXPECT_EQ(report.candidates[1].rnti_class, NR_BLIND_RNTI_CLASS_TC);
  EXPECT_TRUE(report.candidates[0].plausible);
  EXPECT_TRUE(report.candidates[1].plausible);
  EXPECT_NE(report.candidates[0].num_rb, report.candidates[1].num_rb);
  EXPECT_FALSE(out.plausible);
  EXPECT_EQ(out.num_rb, 0);
  EXPECT_EQ(out.num_symbols, 0);
  EXPECT_EQ(out.payload, payload);
  EXPECT_EQ(out.rnti, rnti);
  EXPECT_EQ(out.mismatched_bits, manual.mismatched_bits);
  ASSERT_NE(out.reject_reason, nullptr);
  EXPECT_NE(std::string(out.reject_reason).find("ambiguous"), std::string::npos);
  EXPECT_FALSE(nr_pdcch_blind_decode_10_mode(true, llr.data(), kAggregationLevel,
      len, &ctx, 1, 0xffef, &opts, &out, nullptr));
}

TEST_F(BlindPdcchTest, AutoDci10UniqueCandidateMatchesManualAcrossContexts) {
  for (uint16_t bwp : {24, 48, 106, 273}) {
    for (unsigned rv = 0; rv < 4; ++rv) {
      Dci10Gt gt;
      gt.riv = 2 * bwp + 3; // three PRBs, start 3, independently packed
      gt.tda = rv % 2;
      gt.mcs = 7 + rv;
      gt.ndi = rv % 2;
      gt.rv = rv;
      gt.harq_pid = 3 + 4 * rv;
      const uint16_t len = nr_pdcch_blind_dci10_size(bwp);
      const auto payload = PackDci10Crnti(gt, RivBitsFor(bwp));
      auto llr = EncodeToLLR(payload, 0x4601, len, kAggregationLevel, 40.0, rng_);
      auto ctx = Dci10Ctx(NR_BLIND_SS_UE_SPECIFIC, bwp);
      auto opts = OptsWithTdaLists();
      nr_pdcch_blind_result_t automatic{}, manual{};
      nr_dci10_interpretation_report_t report{};
      ASSERT_TRUE(nr_pdcch_blind_decode_and_extract_10(llr.data(), kAggregationLevel,
          len, &ctx, 1, 0xffef, &opts, &manual));
      ASSERT_TRUE(nr_pdcch_blind_decode_10_mode(true, llr.data(), kAggregationLevel,
          len, &ctx, 1, 0xffef, &opts, &automatic, &report));
      EXPECT_EQ(report.attempted, 1u);
      EXPECT_EQ(report.surviving, 1u);
      EXPECT_EQ(report.unique_candidate, 0);
      EXPECT_EQ(report.state, NR_DCI_UNRESOLVED);
      EXPECT_EQ(automatic.start_rb, 3);
      EXPECT_EQ(automatic.num_rb, 3);
      EXPECT_EQ(automatic.mcs, gt.mcs);
      EXPECT_EQ(automatic.rv, gt.rv);
      EXPECT_EQ(automatic.harq_pid, gt.harq_pid);
      EXPECT_EQ(automatic.start_rb, manual.start_rb);
      EXPECT_EQ(automatic.num_rb, manual.num_rb);
      EXPECT_EQ(automatic.start_symbol, manual.start_symbol);
      EXPECT_EQ(automatic.num_symbols, manual.num_symbols);
      EXPECT_EQ(automatic.dl_dmrs_symb_pos, manual.dl_dmrs_symb_pos);
      EXPECT_EQ(automatic.dmrs_ports, manual.dmrs_ports);
      EXPECT_EQ(automatic.n_dmrs_cdm_groups, manual.n_dmrs_cdm_groups);
      EXPECT_EQ(automatic.nscid, manual.nscid);
      EXPECT_EQ(automatic.mapping_type, manual.mapping_type);
      EXPECT_EQ(automatic.mcs_table, manual.mcs_table);
      EXPECT_EQ(automatic.ndi, manual.ndi);
      EXPECT_EQ(automatic.tb_scaling, manual.tb_scaling);
    }
  }
}

TEST_F(BlindPdcchTest, AutoDci10RetainsEveryRejectionReason) {
  constexpr uint16_t bwp = 48;
  Dci10Gt gt;
  gt.riv = 100;
  gt.reserved = 1;
  auto ctx = Dci10Ctx(NR_BLIND_SS_COMMON, bwp);
  ctx.rnti_class_mask = 1u << NR_BLIND_RNTI_CLASS_RA;
  auto opts = OptsWithTdaLists();
  const uint16_t len = nr_pdcch_blind_dci10_size(bwp);
  auto llr = EncodeToLLR(PackDci10Ra(gt, RivBitsFor(bwp)), 0x11,
                         len, kAggregationLevel, 40.0, rng_);
  nr_pdcch_blind_result_t out{};
  nr_dci10_interpretation_report_t report{};
  EXPECT_FALSE(nr_pdcch_blind_decode_10_mode(true, llr.data(), kAggregationLevel,
      len, &ctx, 1, 0xffef, &opts, &out, &report));
  EXPECT_EQ(report.state, NR_DCI_REJECTED);
  ASSERT_EQ(report.attempted, 1u);
  EXPECT_EQ(report.surviving, 0u);
  EXPECT_EQ(report.unique_candidate, -1);
  EXPECT_EQ(report.candidates[0].rnti_class, NR_BLIND_RNTI_CLASS_RA);
  ASSERT_NE(report.candidates[0].reject_reason, nullptr);
  EXPECT_NE(std::string(report.candidates[0].reject_reason).find("reserved bits"), std::string::npos);
}

TEST_F(BlindPdcchTest, AutoDci10KeepsUlBitsAndClearsReusedReports) {
  constexpr uint16_t bwp = 48;
  Dci10Gt gt;
  gt.riv = 5;
  const uint16_t len = nr_pdcch_blind_dci10_size(bwp);
  const uint64_t payload = PackDci10Crnti(gt, RivBitsFor(bwp), 0);
  auto llr = EncodeToLLR(payload, 0x4601, len, kAggregationLevel, 40.0, rng_);
  auto ctx = Dci10Ctx(NR_BLIND_SS_UE_SPECIFIC, bwp);
  auto opts = OptsWithTdaLists();
  nr_pdcch_blind_result_t out{};
  nr_dci10_interpretation_report_t report{};
  EXPECT_FALSE(nr_pdcch_blind_decode_10_mode(true, llr.data(), kAggregationLevel,
      len, &ctx, 1, 0xffef, &opts, &out, &report));
  EXPECT_EQ(out.payload, payload);
  EXPECT_EQ(out.rnti, 0x4601);
  EXPECT_EQ(report.state, NR_DCI_REJECTED);
  EXPECT_EQ(report.attempted, 1u);
  EXPECT_FALSE(nr_pdcch_blind_decode_10_mode(true, llr.data(), kAggregationLevel,
      len, nullptr, 1, 0xffef, &opts, &out, &report));
  EXPECT_EQ(report.state, NR_DCI_REJECTED);
  EXPECT_EQ(report.attempted, 0u);
  EXPECT_EQ(report.surviving, 0u);
  EXPECT_EQ(report.unique_candidate, -1);
}

#include <cstddef>
extern "C" {
#include "nr_pdcch_ul_field_sweep.h"
}
static nr_pdcch_blind_ul_opts_t FeedbackIdentityOpts()
{
  auto o=LiveUlOpts();
  nr_pdcch_ul_field_widths_t widths{};
  widths.harq_pid_bits=4; widths.dai1_bits=1; widths.antenna_ports_bits=2;
  widths.srs_request_bits=2; widths.dmrs_seq_init_bits=1;
  nr_hyp_t h{}; h.len=sizeof(widths); memcpy(h.bytes,&widths,sizeof(widths));
  nr_pdcch_ul_field_sweep_apply(&h,&o);
  return o;
}
static bool FeedbackIdentityGrant(const nr_pdcch_blind_ul_opts_t &opts, int observation,
                                   nr_pdcch_blind_ul_result_t *grant, nr_pdcch_blind_ul_result_t *truth)
{
  const int i=observation%8;
  UlGroundTruth gt; gt.riv=273*(i+1); gt.mcs=i+2; gt.harq_pid=i; gt.ndi=i%2;
  const auto len=nr_pdcch_blind_dci01_size(&opts);
  const auto payload=PackUlPayload(gt,opts);
  const bool valid=nr_pdcch_blind_extract_01(payload,len,gt.rnti,&opts,truth);
  EXPECT_TRUE(valid) << "independently packed fixture must be interpretable";
  if(!valid) return false;
  return nr_pdcch_ul_discovery_grant(&opts,len,gt.rnti,payload,grant);
}
static bool FeedbackIdentityWaveformMatches(const nr_pdcch_blind_ul_result_t &g,
                                            const nr_pdcch_blind_ul_result_t &t)
{
  return g.start_rb==t.start_rb && g.num_rb==t.num_rb && g.bwp_size==t.bwp_size
      && g.tda_index==t.tda_index && g.start_symbol==t.start_symbol && g.num_symbols==t.num_symbols
      && g.k2==t.k2 && g.mcs==t.mcs && g.mcs_table==t.mcs_table && g.rv==t.rv
      && g.ul_dmrs_symb_pos==t.ul_dmrs_symb_pos && g.dmrs_ports==t.dmrs_ports
      && g.n_dmrs_cdm_groups==t.n_dmrs_cdm_groups && g.nscid==t.nscid
      && g.nrOfLayers==t.nrOfLayers && g.transform_precoding==t.transform_precoding;
}
static nr_pdcch_blind_ul_result_t FeedbackIdentityPrime(const nr_pdcch_blind_ul_opts_t &opts)
{
  nr_pdcch_blind_ul_result_t grant{},truth{};
  bool got=false;
  for(int i=0;i<8;++i) got=FeedbackIdentityGrant(opts,i,&grant,&truth);
  EXPECT_TRUE(got);
  return grant;
}
static nr_pdcch_blind_ul_result_t FeedbackIdentityTrain(const nr_pdcch_blind_ul_opts_t &opts)
{
  nr_pdcch_blind_ul_result_t grant{},truth{};
  int settled_samples=0;
  for(int i=0;i<12000;++i) {
    if(!FeedbackIdentityGrant(opts,i,&grant,&truth)) continue;
    nr_pdcch_ul_discovery_feedback(&grant,FeedbackIdentityWaveformMatches(grant,truth));
    if(nr_pdcch_ul_discovery_snapshot().width_winners>0 && ++settled_samples==64) break;
  }
  return grant;
}
TEST_F(BlindPdcchTest, UlFeedbackOwnershipSurvivesConvergence) {
  nr_pdcch_ul_discovery_reset();
  const auto opts=FeedbackIdentityOpts();
  const auto grant=FeedbackIdentityTrain(opts);
  ASSERT_GT(grant.hyp_generation,0u);
  EXPECT_GE(grant.width_hyp_class,0);
  EXPECT_EQ(grant.interp_hyp_class,-1);
  EXPECT_GT(nr_pdcch_ul_discovery_snapshot().width_winners,0);
  const auto before=nr_pdcch_ul_discovery_snapshot();
  nr_pdcch_ul_discovery_feedback(&grant,true);
  EXPECT_EQ(nr_pdcch_ul_discovery_snapshot().width_trials,before.width_trials+1);
  nr_pdcch_ul_discovery_reset();
}
TEST_F(BlindPdcchTest, UlFeedbackOwnershipRejectsMissingOwnerAfterConvergence) {
  nr_pdcch_ul_discovery_reset();
  const auto opts=FeedbackIdentityOpts();
  auto grant=FeedbackIdentityTrain(opts);
  const auto before=nr_pdcch_ul_discovery_snapshot();
  grant.width_hyp_class=grant.interp_hyp_class=-1;
  nr_pdcch_ul_discovery_feedback(&grant,true);
  const auto after=nr_pdcch_ul_discovery_snapshot();
  EXPECT_EQ(after.width_trials,before.width_trials);
  EXPECT_EQ(after.interp_trials,before.interp_trials);
  EXPECT_EQ(after.rejected_feedback,before.rejected_feedback+1);
  nr_pdcch_ul_discovery_reset();
}
TEST_F(BlindPdcchTest, UlFeedbackOwnershipRejectsTwoOwners) {
  nr_pdcch_ul_discovery_reset();
  const auto opts=FeedbackIdentityOpts();
  auto grant=FeedbackIdentityPrime(opts);
  ASSERT_GE(grant.width_hyp_class,0);
  const auto before=nr_pdcch_ul_discovery_snapshot();
  grant.interp_hyp_class=0;
  nr_pdcch_ul_discovery_feedback(&grant,true);
  const auto after=nr_pdcch_ul_discovery_snapshot();
  EXPECT_EQ(after.width_trials,before.width_trials);
  EXPECT_EQ(after.interp_trials,before.interp_trials);
  EXPECT_EQ(after.rejected_feedback,before.rejected_feedback+1);
  nr_pdcch_ul_discovery_reset();
}
TEST_F(BlindPdcchTest, UlFeedbackOwnershipRejectsAnotherRntiWithCopiedGeneration) {
  nr_pdcch_ul_discovery_reset();
  const auto opts=FeedbackIdentityOpts();
  auto grant=FeedbackIdentityPrime(opts);
  const auto before=nr_pdcch_ul_discovery_snapshot();
  grant.rnti^=1;
  nr_pdcch_ul_discovery_feedback(&grant,true);
  const auto after=nr_pdcch_ul_discovery_snapshot();
  EXPECT_EQ(after.width_trials,before.width_trials);
  EXPECT_EQ(after.rejected_feedback,before.rejected_feedback+1);
  nr_pdcch_ul_discovery_reset();
}
TEST_F(BlindPdcchTest, UlFeedbackOwnershipIgnoresConfigurationPadding) {
  nr_pdcch_ul_discovery_reset();
  auto a=FeedbackIdentityOpts(),b=a;
  const size_t begin=offsetof(nr_pdcch_blind_ul_opts_t,dmrs_typeA_position)+sizeof(a.dmrs_typeA_position);
  const size_t end=offsetof(nr_pdcch_blind_ul_opts_t,tda_count);
  ASSERT_LT(begin,end) << "this ABI fixture needs the alignment padding before tda_count";
  auto bytes=reinterpret_cast<unsigned char*>(&b);
  for(size_t i=begin;i<end;++i) bytes[i]^=0xff;
  ASSERT_NE(memcmp(&a,&b,sizeof(a)),0);
  auto first=FeedbackIdentityPrime(a);
  nr_pdcch_ul_discovery_feedback(&first,true);
  auto same=FeedbackIdentityPrime(b);
  EXPECT_EQ(first.hyp_generation,same.hyp_generation);
  EXPECT_EQ(nr_pdcch_ul_discovery_snapshot().raw_samples,8);
  EXPECT_EQ(nr_pdcch_ul_discovery_snapshot().width_trials,1u);
  nr_pdcch_ul_discovery_reset();
}
TEST_F(BlindPdcchTest, UlFeedbackOwnershipStillSeparatesActualOptionChanges) {
  nr_pdcch_ul_discovery_reset();
  auto a=FeedbackIdentityOpts(),b=a;
  b.mcs_table=(a.mcs_table+1)%3;
  auto first=FeedbackIdentityPrime(a),changed=FeedbackIdentityPrime(b);
  EXPECT_NE(first.hyp_generation,changed.hyp_generation);
  EXPECT_EQ(nr_pdcch_ul_discovery_snapshot().raw_samples,16);
  nr_pdcch_ul_discovery_reset();
}

// ---- CCE-to-REG mapping hypotheses (TS 38.211 7.3.2.2) ------------------------------------------
TEST(MapCandidates, NonInterleavedFirstThenEveryLegalInterleavedMappingWithPciFirst) {
  // Truth on a commercial cell: a 48-RB, 1-symbol dedicated CORESET interleaved with L=6, R=2,
  // shift = PCI (CORESET#0-style). The hypothesis list must contain it, must not seed it as the
  // answer (non-interleaved comes first), and must try the PCI's residue before any other shift.
  nr_pdcch_map_cand_t c[512];
  const int n = nr_pdcch_map_candidates(48, 1, 2, c, 512);
  ASSERT_GT(n, 1);
  EXPECT_EQ(c[0].bundle, 0);
  int truth = -1, first_62 = -1;
  for (int i = 0; i < n; i++) {
    const int nreg = 48, L = c[i].bundle;
    if (L == 0) { EXPECT_EQ(i, 0); continue; }
    EXPECT_TRUE(L == 2 || L == 6);                       // duration 1: L in {2, 6}
    EXPECT_EQ(nreg % (L * c[i].interleaver), 0);         // C integer
    EXPECT_LT(c[i].shift, nreg / L);                     // reduced modulo N_REG/L
    if (L == 6 && c[i].interleaver == 2) {
      if (first_62 < 0) first_62 = i;
      if (c[i].shift == 2 % (nreg / 6)) truth = i;
    }
  }
  ASSERT_GE(truth, 0);
  EXPECT_EQ(truth, first_62);                            // PCI residue is the first shift tried
  EXPECT_EQ(c[first_62 + 1].shift, 0);                   // 0 second
  // every (L, R, shift) distinct
  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++)
      EXPECT_FALSE(c[i].bundle == c[j].bundle && c[i].interleaver == c[j].interleaver && c[i].shift == c[j].shift);
  // count: L=2: R in {2,3,6} legal (48 % 4, % 6, % 12 == 0) -> 3 x 24 shifts; L=6: R=2 (48%12) legal,
  // R=3 (48%18) not, R=6 (48%36) not -> 8 shifts. 1 + 72 + 8.
  EXPECT_EQ(n, 1 + 3 * 24 + 8);
}

TEST(MapCandidates, MacroWideCoresetReachesBundleSixWithinFirstPass) {
  // Macro PCI 64, 216 RB x 2 symbols: 1 + 3*216 (L=2) + 3*72 (L=6) = 865 legal. The old
  // one-(L,R)-at-a-time order put every L=6 entry past index 648, beyond the 512 runtime cap.
  nr_pdcch_map_cand_t c[1024];
  const int n = nr_pdcch_map_candidates(216, 2, 64, c, 1024);
  EXPECT_EQ(n, 1 + 3 * 216 + 3 * 72);
  int l6r2_pci = -1;
  for (int i = 0; i < n; i++)
    if (c[i].bundle == 6 && c[i].interleaver == 2 && c[i].shift == 64 % 72) l6r2_pci = i;
  ASSERT_GE(l6r2_pci, 0);
  EXPECT_LT(l6r2_pci, 1 + 6 * 2);  // pass 0: PCI residue + 0 for each of the 6 legal (L, R)
  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++)
      EXPECT_FALSE(c[i].bundle == c[j].bundle && c[i].interleaver == c[j].interleaver && c[i].shift == c[j].shift);
}

TEST(MapCandidates, DurationThreeUsesBundleThreeAndSix) {
  nr_pdcch_map_cand_t c[512];
  const int n = nr_pdcch_map_candidates(24, 3, 100, c, 512);
  ASSERT_GT(n, 1);
  for (int i = 1; i < n; i++)
    EXPECT_TRUE(c[i].bundle == 3 || c[i].bundle == 6);
  // 72 REGs: L=3 -> 24 bundles, R in {2,3,6} all legal; L=6 -> 12 bundles, R in {2,3,6} all legal
  EXPECT_EQ(n, 1 + 3 * 24 + 3 * 12);
  EXPECT_EQ(c[1].shift, 100 % 24);
}

TEST(MapCandidates, ANonInterleavedCellNeedsNoDwellBeyondHypothesisZero) {
  // The OAI rfsim / srsRAN dedicated CORESETs are non-interleaved: hypothesis 0 is exactly the
  // config those cells decode with (bundle 0 = demapper identity path), so they lock with no
  // added dwell whatever the interleaved tail holds.
  nr_pdcch_map_cand_t c[8];
  ASSERT_GE(nr_pdcch_map_candidates(240, 1, 2, c, 8), 1);
  EXPECT_EQ(c[0].bundle, 0);
  EXPECT_EQ(c[0].interleaver, 0);
  EXPECT_EQ(c[0].shift, 0);
}

// ---- R31 discovery-stall fixes (sa-discovery-stall.md) ----------------------------------------

// Real current signatures (openair1/PHY/NR_REFSIG/nr_refsig.h), declared by hand (same convention
// as nr_pdcch_coreset_map_test.cc) so this file doesn't need to pull in nr_refsig.h's own includes.
// A linkage-specification with braces is only legal at namespace scope, hence file scope here
// rather than inside the test body.
extern "C" {
uint32_t* nr_gold_pdcch(int N_RB_DL, int symbols_per_slot, unsigned short scrambling_id, int slot, int symbol);
void nr_pdcch_dmrs_ref(const uint32_t* gold, c16_t* pilot, unsigned short nb_rb_coreset);
}

// Shared by both DiscoveryGates.Gate2* tests below: build a 48-PRB symbol with exactly one real,
// correctly-generated PDCCH DM-RS window (occupied_rb_offset) and drive
// nr_pdcch_blind_monitor_autodiscover_step() with it, same (slot, symbol) every call, until it
// converges or max_calls is exhausted. Returns the call count at convergence, or -1.
static int RunSparseDiscoveryToConvergence(int occupied_rb_offset, int max_calls) {
  const int n_rb_carrier = 48, ofdm_symbol_size = 512, first_carrier_offset = 10;
  const uint16_t scrambling_id = 2;
  const int slot = 3, symbol = 0;

  std::vector<c16_t> rxdataF(ofdm_symbol_size, {0, 0});
  std::mt19937 rng(1000 + occupied_rb_offset);
  std::normal_distribution<double> noise(0.0, 8.0);
  for (auto& s : rxdataF) {
    s.r = (int16_t)std::lround(noise(rng));
    s.i = (int16_t)std::lround(noise(rng));
  }
  const int pilot_rb_count = occupied_rb_offset + 6;
  uint32_t* gold = nr_gold_pdcch(n_rb_carrier, 14, scrambling_id, slot, symbol);
  std::vector<c16_t> pilot(pilot_rb_count * 3);
  nr_pdcch_dmrs_ref(gold, pilot.data(), (unsigned short)pilot_rb_count);
  for (int rb = occupied_rb_offset; rb < occupied_rb_offset + 6; rb++) {
    for (int p = 0; p < 3; p++) {
      const int k = (first_carrier_offset + rb * 12 + 1 + 4 * p) % ofdm_symbol_size;
      rxdataF[k].r = (int16_t)pilot[rb * 3 + p].r;
      rxdataF[k].i = (int16_t)(-pilot[rb * 3 + p].i);
    }
  }

  for (int i = 1; i <= max_calls; i++) {
    if (nr_pdcch_blind_monitor_autodiscover_step(rxdataF.data(), ofdm_symbol_size, n_rb_carrier,
                                                 first_carrier_offset, scrambling_id, slot, symbol, (uint32_t)i))
      return i;
  }
  return -1;
}

// Both DiscoveryGates.Gate2* tests below park CSS0 on window 7 of the 48-PRB/8-window carrier
// (RunSparseDiscoveryToConvergence's occupied windows are always < 7), owning that global state
// explicitly rather than depending on suite order -- see Gate2SparseBackgroundDominantWindowConverges's
// own comment for why (measured: fails when run as part of the full binary otherwise).
static void ParkCss0OnWindow7() {
  ASSERT_TRUE(nr_pdcch_blind_monitor_autoconf_css0(6, 1, 42, 0, 40, 0, 2, 0, 1, 2, 0, 0));
}

TEST(DiscoveryGates, Gate1EmptyBankNeverBlocksTheOnOccasionFallback) {
  // nr_pdcch_blind_monitor_rt.c's process_body() used to `if (bank_count() == 0) return;` right
  // after the discovery step, which made run_occasion()'s own bank_count()==0 branch (the cheap
  // CORESET#0-USS/RAR-anchor fallback -- "search it before spending the occasion on unknown
  // footprints") structurally unreachable for an entire capture. rt.c is RT-only (needs a live
  // PHY_VARS_NR_UE) and is not linked into this binary, so this is the extracted decision point rt.c
  // now calls instead -- it must never block the fallback, empty bank or not.
  EXPECT_FALSE(nr_pdcch_blind_monitor_discovery_block_early_return(0));
  EXPECT_FALSE(nr_pdcch_blind_monitor_discovery_block_early_return(1));
  EXPECT_FALSE(nr_pdcch_blind_monitor_discovery_block_early_return(5));
}

// Bound shared by both Gate2* tests below: MIN_ORACLE_DWELLS(8) dwells * AUTODISCOVER_OBS_CALLS(1000)
// /dwell = 8000 calls is the fastest the existing per-dwell floor allows even with Gate 2 fixed. The
// pre-fix code could only clear the min-bg gate via AUTODISCOVER_MAX_OBS_CALLS(400000)/dwell --
// 400000x more calls per dwell -- so converging within a low-thousands call budget is itself the
// regression check.
static constexpr int kGate2MaxCalls = 9000;
static constexpr int kGate2ExpectedCallBound = 8200;

TEST(DiscoveryGates, Gate2SparseBackgroundDominantWindowConverges) {
  // Live evidence (sa-discovery-stall.md): with sparse traffic one window climbs 16->138 hits over
  // 45000 calls while every other window stays near 0, so the ISAC_DISCOVER_MIN_BG=3 whole-carrier
  // MEDIAN never becomes estimable -- zero dwells ever complete, however much evidence the true
  // window already has. This test reproduces that shape (one genuinely occupied window, a carrier
  // otherwise never producing a hit -- pure noise essentially never clears
  // nr_pdcch_coreset_map_scan()'s own correlation floor) and checks the K/N dominance bypass lets
  // discovery converge anyway.
  ParkCss0OnWindow7();
  nr_pdcch_blind_monitor_autodiscover_reset();

  const int occupied_rb_offset = 18; // one 6-RB window, not aligned to a carrier edge
  const int calls = RunSparseDiscoveryToConvergence(occupied_rb_offset, kGate2MaxCalls);
  ASSERT_GT(calls, 0) << "did not converge within " << kGate2MaxCalls << " calls";
  EXPECT_LE(calls, kGate2ExpectedCallBound);
  EXPECT_EQ(nr_pdcch_blind_monitor_get_cfg()->coreset_rb_offset, occupied_rb_offset);
}

TEST(DiscoveryGates, Gate2ReconvergesAfterPriorDiscoveryInTheSameProcess) {
  // Fix round 1 (controller ruling): nr_pdcch_blind_monitor_autodiscover_reset() used to leave the
  // LONG-TERM dwell state (s_lt_ndwell/s_lt_hits/s_lt_dwells/s_lt_rnti) untouched -- only ever
  // accumulated, never cleared. Gate 2's dominance bypass and the pre-existing MIN_ORACLE_DWELLS
  // seed selection both key off that state (recurrence_floor = (s_lt_ndwell+4)/5), so a stale
  // s_lt_ndwell/s_lt_dwells left over from an EARLIER discovery could let a second discovery's seed
  // selection latch onto the FIRST run's window (already recorded enough stale dwells to clear
  // recurrence_floor) instead of genuinely re-discovering the new one -- order-dependent under
  // --gtest_shuffle/sharding, and in production the same failure mode for a real re-discovery (BWP
  // switch, cell change) seeding off a stale footprint.
  //
  // Run discovery twice in the same process, resetting in between, with a DIFFERENT occupied window
  // each time. If the reset doesn't clear the long-term state, the second run either converges
  // suspiciously fast on the WRONG (first) window, or takes longer/fails as stale evidence
  // interferes -- either way EXPECT_EQ on the second window below catches it.
  ParkCss0OnWindow7();
  nr_pdcch_blind_monitor_autodiscover_reset();
  const int first_window_rb_offset = 18;
  const int calls1 = RunSparseDiscoveryToConvergence(first_window_rb_offset, kGate2MaxCalls);
  ASSERT_GT(calls1, 0) << "first discovery did not converge";
  ASSERT_EQ(nr_pdcch_blind_monitor_get_cfg()->coreset_rb_offset, first_window_rb_offset);

  ParkCss0OnWindow7();
  nr_pdcch_blind_monitor_autodiscover_reset();
  const int second_window_rb_offset = 0; // different window (window 0, not window 3)
  const int calls2 = RunSparseDiscoveryToConvergence(second_window_rb_offset, kGate2MaxCalls);
  ASSERT_GT(calls2, 0) << "second discovery (after reset) did not converge";
  EXPECT_LE(calls2, kGate2ExpectedCallBound) << "second discovery took longer than a fresh one should";
  EXPECT_EQ(nr_pdcch_blind_monitor_get_cfg()->coreset_rb_offset, second_window_rb_offset)
      << "discovered the FIRST run's window instead of this run's -- long-term dwell state leaked "
         "across the reset";
}

TEST(DmrsRankMapping, MatchesProductionDemapperAcrossLegalMappings) {
  // Label every data RE with its physical RB and symbol. The actual production demapper
  // independently determines the ordered resource list; no duplicated forward mapper fixture.
  for(int dur=1;dur<=3;dur++) for(int span:{6,24,48,270})
    for(int bundle:{0,2,3,6}) for(int inter:{2,3,6}) for(int shift:{0,1,17})
      for(int agg:{1,2,4,8,16}) {
        if((bundle==0 && (inter!=2 || shift)) || (bundle && (bundle%dur || (dur<3&&bundle==3)
            || span*dur%(bundle*inter)))) continue;
        const int ncc=span*dur/6;
        if(agg>ncc) continue;
        for(int cc:{0,((ncc-agg)/agg)*agg}) {
          uint16_t rbs[96];
          const int n=nr_pdcch_candidate_rbs(span,dur,bundle,inter,shift,cc,agg,rbs,96);
          ASSERT_EQ(n,agg*6/dur);
          std::vector<c16_t> llr(span*dur*9),out(agg*54);
          for(int s=0;s<dur;s++) for(int rb=0;rb<span;rb++) for(int q=0;q<9;q++)
            llr[(s*span+rb)*9+q]={(int16_t)rb,(int16_t)s};
          uint16_t cce=cc; uint8_t al=agg;
          nr_pdcch_demapping_deinterleaving(span,llr.data(),out.data(),dur,bundle,inter,shift,1,&cce,&al,span*9);
          for(int s=0;s<dur;s++) for(int j=0;j<n;j++) for(int q=0;q<9;q++) {
            const auto v=out[(s*n+j)*9+q];
            ASSERT_EQ(v.r,rbs[j]); ASSERT_EQ(v.i,s);
          }
        }
      }
}

TEST_F(BlindPdcchTest, SixRbAlOneIsADecodableCoreset) {
  const uint16_t len=40,rnti=0x1234;
  const uint64_t payload=0x8123456789ULL;
  auto bits=EncodeToLLR(payload,rnti,len,1,40.0,rng_);
  ASSERT_EQ(bits.size(),108u);
  std::vector<c16_t> llr(54),out(54);
  for(int i=0;i<54;i++) llr[i]={bits[2*i],bits[2*i+1]};
  uint16_t cce=0; uint8_t al=1;
  nr_pdcch_demapping_deinterleaving(6,llr.data(),out.data(),1,0,0,0,1,&cce,&al,54);
  nr_pdcch_blind_raw_result_t decoded={};
  ASSERT_TRUE(nr_pdcch_blind_decode_raw_11((int16_t*)out.data(),1,len,rnti,rnti,&decoded));
  EXPECT_EQ(decoded.rnti,rnti); EXPECT_EQ(decoded.payload,payload);
}

TEST_F(BlindPdcchTest, RawCss0ControlHasNoDlUlIndicator) {
  const int len=39;
  for (uint64_t payload : {UINT64_C(0x12345678), (UINT64_C(1)<<(len-1))|UINT64_C(0x12345678)}) {
    auto llr=EncodeToLLR(payload,0xffff,len,4,40.0,rng_);
    nr_pdcch_blind_raw_result_t raw{};
    ASSERT_TRUE(nr_pdcch_blind_decode_raw(llr.data(),4,len,1,65535,false,&raw));
    EXPECT_EQ(raw.rnti,0xffff); EXPECT_EQ(raw.payload,payload);
    EXPECT_EQ(nr_pdcch_blind_decode_raw_11(llr.data(),4,len,1,65535,&raw),bool(payload>>(len-1)));
  }
}

TEST(PdcchReplay, OtaCss0DecoderContract) {
  const char *path=getenv("ISAC_PDCCH_REPLAY_INPUT");
  if (!path || !*path) GTEST_SKIP()<<"Set ISAC_PDCCH_REPLAY_INPUT to a completed diagnostic capture";
  FILE *f=fopen(path,"rb");
  ASSERT_NE(f,nullptr);
  unsigned controls=0,hypotheses=0,without_indicator=0,negative=0;
  for (;;) {
    nr_pdcch_discovery_replay_t h{};
    const size_t bytes=fread(&h,1,sizeof(h),f);
    if (!bytes && feof(f)) break;
    ASSERT_EQ(bytes,sizeof(h))<<"VOID truncated header";
    ASSERT_EQ(h.magic,NR_PDCCH_REPLAY_MAGIC); ASSERT_TRUE(h.version==1u || h.version==2u);
    ASSERT_EQ(h.header_bytes,sizeof(h)); ASSERT_GE(h.span,6u); ASSERT_LE(h.span,270u);
    ASSERT_GE(h.duration,1u); ASSERT_LE(h.duration,3u);
    ASSERT_LE(h.first_symbol+h.duration,14u); ASSERT_LE(h.fft_size,8192u);
    ASSERT_GE(h.fft_size,128u); ASSERT_LE(h.n_candidates,64u);
    ASSERT_EQ(h.grid_count,h.span*9*h.duration); ASSERT_LE(h.expected_re,54u*16);
    ASSERT_TRUE(h.kind==1 || h.kind==2);
    std::vector<c16_t> grid(h.grid_count),fft(h.fft_size*h.duration),expected(h.expected_re);
    ASSERT_EQ(fread(grid.data(),sizeof(c16_t),grid.size(),f),grid.size());
    ASSERT_EQ(fread(fft.data(),sizeof(c16_t),fft.size(),f),fft.size());
    ASSERT_EQ(fread(expected.data(),sizeof(c16_t),expected.size(),f),expected.size());
    if (h.kind==2) { ++hypotheses; continue; }
    ASSERT_LT(h.expected_index,h.n_candidates); ASSERT_EQ(h.expected_rnti,0xffffu);
    uint16_t cce=h.cce[h.expected_index]; uint8_t al=h.al[h.expected_index];
    ASSERT_TRUE(al==1 || al==2 || al==4 || al==8 || al==16);
    ASSERT_LE((cce+al)*6,h.span*h.duration); ASSERT_EQ(h.expected_re,54u*al);
    std::vector<c16_t> demapped(h.expected_re);
    nr_pdcch_demapping_deinterleaving(h.span,grid.data(),demapped.data(),h.duration,h.bundle,
        h.interleaver,h.shift,1,&cce,&al,h.span*9);
    ASSERT_EQ(memcmp(demapped.data(),expected.data(),expected.size()*sizeof(c16_t)),0)
        <<"source="<<h.source_slot<<" demapper does not reproduce live candidate";
    std::vector<int16_t> llr(al*108);
    nr_pdcch_unscrambling(demapped.data(),h.scrambling_rnti,al*108,h.dmrs_id,llr.data());
    nr_pdcch_blind_raw_result_t raw{};
    ASSERT_TRUE(nr_pdcch_blind_decode_raw(llr.data(),al,h.expected_length,1,65535,false,&raw))
        <<"source="<<h.source_slot<<" reject="<<(raw.reject_reason?raw.reject_reason:"");
    ASSERT_EQ(raw.rnti,h.expected_rnti); ASSERT_EQ(raw.payload,h.expected_payload);
    // Exercise unknown-length traversal through the same raw polar/CRC core.
    unsigned exact_matches=0;
    for (int len=1;len<=63;++len) {
      nr_pdcch_blind_raw_result_t trial{};
      bool ok=nr_pdcch_blind_decode_raw(llr.data(),al,len,1,65535,false,&trial);
      if(ok && trial.rnti==h.expected_rnti && trial.payload==h.expected_payload) {
        ++exact_matches; EXPECT_EQ(len,h.expected_length);
      }
    }
    EXPECT_EQ(exact_matches,1u)<<"source="<<h.source_slot;
    const bool dl_bit=(h.expected_payload>>(h.expected_length-1))&1;
    if(!dl_bit) ++without_indicator;
    EXPECT_EQ(nr_pdcch_blind_decode_raw_11(llr.data(),al,h.expected_length,1,65535,&raw),dl_bit);
    // Counterfactual on the IDENTICAL samples: wrong data scrambling must not
    // reproduce the trusted tuple. A different plausible CRC is not a success.
    nr_pdcch_unscrambling(demapped.data(),h.scrambling_rnti^0x1234,al*108,h.dmrs_id,llr.data());
    const bool wrong=nr_pdcch_blind_decode_raw(llr.data(),al,h.expected_length,1,65535,false,&raw);
    ASSERT_FALSE(wrong && raw.rnti==h.expected_rnti && raw.payload==h.expected_payload);
    ++negative; ++controls;
    printf("PDCCHREPLAY control source=%lu span=%u L=%u len=%u rnti=0x%04x payload=0x%016lx PASS\n",
           (unsigned long)h.source_slot,h.span,al,h.expected_length,h.expected_rnti,(unsigned long)h.expected_payload);
  }
  fclose(f);
  EXPECT_GE(controls,8u)<<"VOID: too few independent-slot CSS0 controls";
  EXPECT_GT(hypotheses,0u)<<"VOID: no dedicated hypotheses captured";
  printf("PDCCHREPLAY controls=%u unknown_hypotheses=%u no_indicator_bit=%u wrong_scrambling_rejected=%u\n",
         controls,hypotheses,without_indicator,negative);
}

TEST(DciSweepBudget, ResumesWithoutRepeatingOrInventingEvidence) {
  struct Seen { int count[64][5]{}; } seen;
  auto scorer=[](int len,int trial,uint16_t*,uint32_t*,void *p)->bool {
    auto *s=static_cast<Seen*>(p); EXPECT_LT(trial,5); ++s->count[len][trial]; return false;
  };
  nr_pdcch_dci_length_sweep_state_t state{};
  for(int call=0;call<7;++call) {
    const auto before=state.decodes;
    nr_pdcch_dci_length_sweep_feed_budget(&state,scorer,&seen,5,30,33,0,0,3);
    EXPECT_LE(state.decodes-before,3u);
    if(call<6) EXPECT_EQ(state.occasions_fed,0);
  }
  EXPECT_EQ(state.occasions_fed,1); EXPECT_EQ(state.decodes,20u);
  for(int len=30;len<=33;++len) {
    EXPECT_EQ(state.trials[len],5);
    for(int trial=0;trial<5;++trial) EXPECT_EQ(seen.count[len][trial],1);
  }
  nr_pdcch_dci_length_sweep_reset(&state);
  EXPECT_EQ(state.resume_len,0); EXPECT_EQ(state.resume_trial,0);
}
TEST(DciSweepBudget, ExpiredDeadlineDoesNotFabricateAnOccasion) {
  int calls=0;
  auto scorer=[](int,int,uint16_t*,uint32_t*,void *p)->bool { ++*static_cast<int*>(p); return false; };
  nr_pdcch_dci_length_sweep_state_t state{};
  nr_pdcch_dci_length_sweep_feed_budget(&state,scorer,&calls,8,30,63,0,1,0);
  EXPECT_EQ(calls,0); EXPECT_EQ(state.decodes,0u); EXPECT_EQ(state.occasions_fed,0);
  nr_pdcch_dci_length_sweep_feed_budget(&state,scorer,&calls,2,30,63,0,0,1);
  EXPECT_EQ(calls,1); EXPECT_EQ(state.trials[30],1); EXPECT_EQ(state.occasions_fed,0);
}

// Timing workload only: immutable OTA soft samples, every legal contiguous width.
// Includes DMRS ranking, demapping, deadline-limited length sweep and two ordinary
// candidate decode attempts. Excludes live FFT, queueing and PHY dispatch; OTA
// DISCOVERYLAT is the separate whole-occasion acceptance measurement.
TEST(PdcchReplay, BudgetTimingEveryWidth) {
  const char *path=getenv("ISAC_PDCCH_REPLAY_BENCH");
  if(!path || !*path) GTEST_SKIP()<<"Set ISAC_PDCCH_REPLAY_BENCH for the recorded workload timing test";
  FILE *f=fopen(path,"rb"); ASSERT_NE(f,nullptr);
  nr_pdcch_discovery_replay_t h{};
  std::vector<c16_t> recorded,fft;
  bool found=false;
  while(fread(&h,sizeof(h),1,f)==1) {
    ASSERT_EQ(h.magic,NR_PDCCH_REPLAY_MAGIC); ASSERT_EQ(h.header_bytes,sizeof(h));
    ASSERT_LE(h.grid_count,270u*9*3); ASSERT_LE(h.fft_size*h.duration,8192u*3);
    recorded.resize(h.grid_count); fft.resize(h.fft_size*h.duration);
    ASSERT_EQ(fread(recorded.data(),sizeof(c16_t),recorded.size(),f),recorded.size());
    ASSERT_EQ(fread(fft.data(),sizeof(c16_t),fft.size(),f),fft.size());
    ASSERT_EQ(fseek(f,h.expected_re*sizeof(c16_t),SEEK_CUR),0);
    if(h.kind==2 && h.span==270 && h.duration==1) { found=true; break; }
  }
  fclose(f); ASSERT_TRUE(found)<<"VOID: no full-width one-symbol OTA grid";
  auto now=[]()->uint64_t { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return uint64_t(t.tv_sec)*1000000000ull+t.tv_nsec; };
  std::vector<c16_t> full_fft(14*h.fft_size);
  memcpy(full_fft.data()+h.first_symbol*h.fft_size,fft.data(),fft.size()*sizeof(c16_t));
  struct Context {
    std::vector<c16_t> *samples;
    std::vector<unsigned> offset;
    std::vector<uint8_t> al;
    uint16_t id;
  };
  auto scorer=[](int len,int trial,uint16_t *rnti,uint32_t *hash,void *opaque)->bool {
    auto &c=*static_cast<Context*>(opaque);
    int16_t llr[16*108];
    nr_pdcch_unscrambling(c.samples->data()+c.offset[trial],0,c.al[trial]*108,c.id,llr);
    nr_pdcch_blind_raw_result_t raw{};
    if(!nr_pdcch_blind_decode_raw_11(llr,c.al[trial],len,1,65519,&raw)) return false;
    *rnti=raw.rnti; *hash=uint32_t(raw.payload)^uint32_t(raw.payload>>32); return true;
  };
  for(int span=6;span<=270;span+=6) {
    uint16_t cce[64]; uint8_t al[64]; int nc=0;
    for(int level : {2,4,8,1})
      for(int c=0;c+level<=span/6 && nc<64;c+=level) { cce[nc]=c; al[nc++]=level; }
    ASSERT_GT(nc,0);
    std::vector<c16_t> grid(recorded.begin(),recorded.begin()+span*9),demapped(64*54*16);
    nr_pdcch_dci_length_sweep_state_t state{};
    std::vector<uint64_t> elapsed; elapsed.reserve(1024);
    Context ctx{&demapped,{}, {},uint16_t(h.dmrs_id)};
    for(int visit=0;visit<1024;++visit) {
      const uint64_t begin=now();
      nr_pdcch_dmrs_rank_grid_t rank{};
      ASSERT_TRUE(nr_pdcch_dmrs_rank_grid(&rank,full_fft.data(),h.fft_size,h.first_carrier_offset,
                    h.carrier_rb,h.dmrs_id,h.slot,h.first_symbol,1,0));
      double scores[64]; uint8_t order[64],kept_al[64]; uint16_t kept_cce[64];
      for(int c=0;c<nc;++c) scores[c]=nr_pdcch_dmrs_candidate_score(&rank,h.offset,span,0,0,0,cce[c],al[c]);
      const int kept=nr_pdcch_dmrs_candidate_order(scores,al,nc,visit,visit%16==0,order);
      unsigned off=0; ctx.offset.clear(); ctx.al.clear();
      for(int c=0;c<kept;++c) {
        kept_cce[c]=cce[order[c]]; kept_al[c]=al[order[c]];
        ctx.offset.push_back(off); ctx.al.push_back(kept_al[c]); off+=54*kept_al[c];
      }
      nr_pdcch_demapping_deinterleaving(span,grid.data(),demapped.data(),1,0,0,0,kept,kept_cce,kept_al,span*9);
      (void)nr_pdcch_dci_length_sweep_feed_budget(&state,scorer,&ctx,kept,30,63,0,begin+350000,0);
      // Ordinary fixed-length candidate decoding remains outside the sweep budget.
      for(int c=0;c<kept;++c) {
        uint16_t rnti; uint32_t hash;
        scorer(44,c,&rnti,&hash,&ctx); scorer(51,c,&rnti,&hash,&ctx);
      }
      elapsed.push_back(now()-begin);
    }
    uint64_t sum=0; for(auto ns:elapsed) sum+=ns;
    std::sort(elapsed.begin(),elapsed.end());
    const double mean_us=sum/(1000.0*elapsed.size());
    const double p99_us=elapsed[(elapsed.size()*99+99)/100-1]/1000.0;
    printf("REPLAYLAT span=%d n=%zu mean_us=%.2f p99_us=%.2f max_us=%.2f decodes=%lu rounds=%d\n",
           span,elapsed.size(),mean_us,p99_us,elapsed.back()/1000.0,(unsigned long)state.decodes,state.occasions_fed);
    EXPECT_LT(mean_us,500.0)<<"span="<<span;
    EXPECT_LT(p99_us,1000.0)<<"span="<<span;
    EXPECT_GT(state.occasions_fed,0)<<"budget starved complete rounds at span="<<span;
  }
}

// Task 15: lanes may scan AL16. ISAC_LANE_ALS=16 must parse (it used to be dropped by the AL8 lane
// cap), and one lane's extracted-RE budget must hold an AL16 candidate: 16 CCE x 6 REG x 9 RE = 864.
TEST(LookaheadLanes, Al16IsAcceptedAndFitsTheLaneBudget) {
  uint8_t v[5] = {0};
  ASSERT_EQ(nr_pdcch_blind_parse_lane_als("16", v), 1);
  EXPECT_EQ(v[0], 16);
  EXPECT_EQ(nr_pdcch_blind_parse_lane_als("1,2,4,8,16", v), 5);
  EXPECT_EQ(v[4], 16);
  EXPECT_EQ(nr_pdcch_blind_parse_lane_als("32,3,0", v), 0);
  EXPECT_EQ(nr_pdcch_blind_parse_lane_als(nullptr, v), 0);
  EXPECT_GE(nr_pdcch_blind_lane_re_budget(), 16 * 6 * 9);
}
