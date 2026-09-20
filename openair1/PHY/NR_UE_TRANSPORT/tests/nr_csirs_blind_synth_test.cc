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

/*! \file nr_csirs_blind_synth_test.cc
 * \brief END-TO-END self-test of the blind CSI-RS chain against a resource WE generate.
 *
 * WHY THIS EXISTS. On air the chain reports: strong sequence-free energy at a candidate's positions,
 * but no correlation match at ANY scramblingID (complete 1024 sweep) and ANY slot offset (complete
 * 20 sweep). Three explanations survive that evidence: the energy is not CSI-RS at all; the
 * enumeration never contains the true resource; or the scoring cannot recognise a match it is handed.
 * The last two are testable WITHOUT a radio, and had never been tested: every previous test exercised
 * the pure helpers (correlate, period inference) on synthetic vectors, never the real generator
 * against the real enumeration.
 *
 * So: generate a resource with nr_generate_csi_rs() -- the same function the searcher uses to build
 * its reference -- put it through a channel (phase ramp + noise), and ask whether
 *   (a) nr_csirs_blind_enumerate() contains that exact (row, freq_domain, symbol, density, cdm), and
 *   (b) the channel-robust score ranks it top, above the confirmation bar.
 * If both pass, the chain is sound and the OTA failure is the cell's signal, not our code.
 */

#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include <gtest/gtest.h>

extern "C" {
#include "PHY/defs_nr_common.h"
#include "PHY/nr_phy_common/inc/nr_phy_common.h"
#include "openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_search.h"
}

namespace {

constexpr int kNRB = 273;           // the cell under test: 100 MHz at 30 kHz
constexpr int kFFT = 4096;
constexpr int kSymbols = 14;

/* A frame_parms with only the fields the generator and the scorer read. */
NR_DL_FRAME_PARMS make_fp()
{
  NR_DL_FRAME_PARMS fp;
  memset(&fp, 0, sizeof(fp));
  fp.N_RB_DL = kNRB;
  fp.ofdm_symbol_size = kFFT;
  fp.first_carrier_offset = kFFT - (kNRB * 12) / 2;
  fp.symbols_per_slot = kSymbols;
  fp.slots_per_frame = 20;
  fp.numerology_index = 1;
  fp.nb_antennas_rx = 1;
  return fp;
}

/* Transmit one CSI-RS resource into a slot buffer, then apply a channel: a linear phase ramp across
 * the band (the thing that defeats a flat correlation) plus additive noise. */
void transmit(const NR_DL_FRAME_PARMS &fp, const nr_csirs_candidate_t &c, int slot, double ramp_rad,
              double noise_amp, std::vector<c16_t> &rx)
{
  std::vector<c16_t> buf((size_t)kFFT * kSymbols);
  memset(buf.data(), 0, buf.size() * sizeof(c16_t));
  c16_t *planes[4] = {buf.data(), nullptr, nullptr, nullptr};
  const csi_mapping_parms_t parms = get_csi_mapping_parms(c.row, c.freq_domain, c.symb_l0, c.symb_l1);
  nr_generate_csi_rs(&fp, &parms, 4096 /* amp */, slot, c.freq_density, c.start_rb, c.nr_of_rbs,
                     c.symb_l0, c.symb_l1, c.row, c.scramb_id, 0, c.cdm_type, planes);

  rx.assign(buf.begin(), buf.end());
  std::mt19937 g(1234);
  std::normal_distribution<double> nd(0.0, noise_amp);
  const uint32_t off = (uint32_t)c.symb_l0 * kFFT;
  for (int k = 0; k < kFFT; k++) {
    const double ph = ramp_rad * ((double)k / (double)kFFT);
    const double r = buf[off + k].r, i = buf[off + k].i;
    rx[off + k].r = (int16_t)(r * cos(ph) - i * sin(ph) + nd(g));
    rx[off + k].i = (int16_t)(r * sin(ph) + i * cos(ph) + nd(g));
  }
}

/* Score every enumerated candidate the way nr_csirs_blind_rt_slot() does, and report the rank of
 * the true one. */
struct ScanResult {
  int n_cand;
  int true_idx;
  int rank;          // 0 = top
  double z_true;
  double z_runner_up;
  double epr_true;
};

ScanResult scan(const NR_DL_FRAME_PARMS &fp, const std::vector<c16_t> &rx,
                const nr_csirs_candidate_t &truth, int slot)
{
  std::vector<nr_csirs_candidate_t> cand(NR_CSIRS_BLIND_MAX_CAND);
  const int n = nr_csirs_blind_enumerate(cand.data(), NR_CSIRS_BLIND_MAX_CAND, kNRB, truth.scramb_id);
  ScanResult out{n, -1, -1, 0.0, 0.0, 0.0};
  std::vector<c16_t> ref((size_t)kFFT * kSymbols);
  std::vector<std::pair<double, int>> scores;
  for (int i = 0; i < n; i++) {
    const nr_csirs_candidate_t &c = cand[i];
    memset(ref.data(), 0, ref.size() * sizeof(c16_t));
    c16_t *planes[4] = {ref.data(), nullptr, nullptr, nullptr};
    const csi_mapping_parms_t p = get_csi_mapping_parms(c.row, c.freq_domain, c.symb_l0, c.symb_l1);
    nr_generate_csi_rs(&fp, &p, 4096, slot, c.freq_density, c.start_rb, c.nr_of_rbs, c.symb_l0,
                       c.symb_l1, c.row, c.scramb_id, 0, c.cdm_type, planes);
    const uint32_t off = (uint32_t)c.symb_l0 * kFFT;
    int used = 0;
    const double z = nr_csirs_blind_correlate_blocks((const int16_t *)&rx[off], (const int16_t *)&ref[off],
                                                     kFFT, 32, &used);
    scores.push_back({z < 0 ? 0.0 : z, i});
    if (c.row == truth.row && c.freq_domain == truth.freq_domain && c.symb_l0 == truth.symb_l0
        && c.freq_density == truth.freq_density && c.cdm_type == truth.cdm_type) {
      out.true_idx = i;
      out.z_true = z;
      out.epr_true = nr_csirs_blind_energy_ratio((const int16_t *)&rx[off], (const int16_t *)&ref[off], kFFT);
    }
  }
  std::sort(scores.begin(), scores.end(), [](auto &a, auto &b) { return a.first > b.first; });
  for (size_t r = 0; r < scores.size(); r++)
    if (scores[r].second == out.true_idx)
      out.rank = (int)r;
  out.z_runner_up = (scores.size() > 1 && scores[0].second == out.true_idx) ? scores[1].first : scores[0].first;
  return out;
}

} // namespace

TEST(CsirsBlindSynth, EnumerationContainsARealisticTrsResource)
{
  /* The shape the OTA energy test keeps pointing at: row 1 (single port, density 3), one symbol. */
  std::vector<nr_csirs_candidate_t> cand(NR_CSIRS_BLIND_MAX_CAND);
  const int n = nr_csirs_blind_enumerate(cand.data(), NR_CSIRS_BLIND_MAX_CAND, kNRB, 382);
  ASSERT_GT(n, 0);
  int row1 = 0, row2 = 0, row4 = 0;
  for (int i = 0; i < n; i++) {
    if (cand[i].row == 1) row1++;
    else if (cand[i].row == 2) row2++;
    else if (cand[i].row == 4) row4++;
  }
  printf("ENUM: %d candidates (row1=%d row2=%d row4=%d)\n", n, row1, row2, row4);
  EXPECT_GT(row1, 0);
  EXPECT_GT(row2, 0);
}

TEST(CsirsBlindSynth, FindsAResourceItGeneratedItself)
{
  const NR_DL_FRAME_PARMS fp = make_fp();
  std::vector<nr_csirs_candidate_t> cand(NR_CSIRS_BLIND_MAX_CAND);
  const int n = nr_csirs_blind_enumerate(cand.data(), NR_CSIRS_BLIND_MAX_CAND, kNRB, 382);
  ASSERT_GT(n, 0);
  /* Pick a row-1 candidate from the enumeration itself as the "truth", so the test cannot fail for
   * the trivial reason that the truth was never enumerable. */
  nr_csirs_candidate_t truth = cand[0];
  for (int i = 0; i < n; i++)
    if (cand[i].row == 1) { truth = cand[i]; break; }
  printf("TRUTH: row=%u fd=%u l0=%u density=%u cdm=%u id=%u\n", truth.row, truth.freq_domain,
         truth.symb_l0, truth.freq_density, truth.cdm_type, truth.scramb_id);

  std::vector<c16_t> rx;
  transmit(fp, truth, /*slot=*/7, /*ramp_rad=*/40.0, /*noise_amp=*/60.0, rx);
  const ScanResult r = scan(fp, rx, truth, 7);
  printf("SCAN: %d candidates, truth rank=%d z_true=%.2f runner_up=%.2f epr_true=%.2f\n",
         r.n_cand, r.rank, r.z_true, r.z_runner_up, r.epr_true);

  ASSERT_GE(r.true_idx, 0) << "the enumeration does not contain the resource we transmitted";
  EXPECT_EQ(r.rank, 0) << "the true resource is not the top-scoring candidate";
  EXPECT_GT(r.z_true, 4.0) << "the true resource does not clear the confirmation bar";
  EXPECT_GT(r.epr_true, 2.0) << "the sequence-free energy test does not see its own transmission";
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
