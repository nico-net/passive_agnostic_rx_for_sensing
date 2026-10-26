/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* CRC arbiter of the blind CSI-RS rate-matching patterns: per-pattern verdicts from decode trials. */
#include "nr_csirs_rm_arbiter.h"
#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include "common/utils/LOG/log.h"
#include "common/config/config_userapi.h"

configmodule_interface_t *uniqCfg = nullptr;
extern "C" {
void exit_function(const char *, const char *, int, const char *, int) { std::abort(); }
}

namespace {
fapi_nr_dl_config_csirs_pdu_rel15_t pat(uint8_t row, uint8_t l0, uint16_t id, bool zp, bool holes)
{
  fapi_nr_dl_config_csirs_pdu_rel15_t p;
  std::memset(&p, 0, sizeof(p));
  p.row = row;
  p.symb_l0 = l0;
  p.freq_domain = 4;
  p.cdm_type = 1;
  p.freq_density = 2;
  p.start_rb = 0;
  p.nr_of_rbs = 272;
  p.scramb_id = id;
  p.csi_type = zp ? 2 : 1;
  if (holes)
    p.measurement_bitmap |= NR_CSIRS_PDU_HOLE_ONLY;
  return p;
}
}  // namespace

class Arb : public ::testing::Test {
 protected:
  void SetUp() override { nr_csi_arb_reset(); }
};

/* a pattern with sequence evidence (NZP) is in by default, and stays in under a single contrary pass */
TEST_F(Arb, SequencePatternIsAppliedByDefault)
{
  auto nzp = pat(4, 4, 2, false, false);
  EXPECT_TRUE(nr_csi_arb_applies(&nzp));
  EXPECT_EQ(nr_csi_arb_verdict(&nzp), NR_CSI_ARB_UNDECIDED);
  EXPECT_FALSE(nr_csi_arb_trial_add(&nzp));
  EXPECT_TRUE(nr_csi_arb_trial_remove(&nzp));
  nr_csi_arb_note(&nzp, NR_CSI_ARB_WITHOUT_WINS);
  EXPECT_TRUE(nr_csi_arb_applies(&nzp)) << "one pass without it is not a verdict";
}

/* two decodes without it passed where decoding with it failed, and nothing in favour: dropped for every slot */
TEST_F(Arb, SequencePatternDroppedOnTwoContraryPasses)
{
  auto nzp = pat(4, 4, 2, false, false);
  nr_csi_arb_note(&nzp, NR_CSI_ARB_WITHOUT_WINS);
  EXPECT_EQ(nr_csi_arb_note(&nzp, NR_CSI_ARB_WITHOUT_WINS), NR_CSI_ARB_DROP);
  EXPECT_FALSE(nr_csi_arb_applies(&nzp));
  /* another pattern is untouched */
  auto other = pat(4, 6, 2, false, false);
  EXPECT_TRUE(nr_csi_arb_applies(&other));
}

/* a hole-only ZP is held out until two decodes with it passed where decoding without it failed */
TEST_F(Arb, HolePatternNeedsProof)
{
  auto zp = pat(4, 8, 0, true, true);
  EXPECT_FALSE(nr_csi_arb_applies(&zp));
  EXPECT_TRUE(nr_csi_arb_trial_add(&zp));
  EXPECT_EQ(nr_csi_arb_note(&zp, NR_CSI_ARB_WITH_WINS), NR_CSI_ARB_UNDECIDED);
  EXPECT_FALSE(nr_csi_arb_applies(&zp));
  EXPECT_EQ(nr_csi_arb_note(&zp, NR_CSI_ARB_WITH_WINS), NR_CSI_ARB_KEEP);
  EXPECT_TRUE(nr_csi_arb_applies(&zp));
  EXPECT_FALSE(nr_csi_arb_trial_add(&zp));
  EXPECT_FALSE(nr_csi_arb_trial_remove(&zp)) << "a proven pattern is not tried away";
}

/* evidence both ways does not decide: the pattern keeps its default */
TEST_F(Arb, ContestedEvidenceDecidesNothing)
{
  auto zp = pat(4, 8, 0, true, true);
  nr_csi_arb_note(&zp, NR_CSI_ARB_WITH_WINS);
  nr_csi_arb_note(&zp, NR_CSI_ARB_WITH_WINS);
  nr_csi_arb_note(&zp, NR_CSI_ARB_WITHOUT_WINS);
  nr_csi_arb_note(&zp, NR_CSI_ARB_WITHOUT_WINS);
  EXPECT_EQ(nr_csi_arb_verdict(&zp), NR_CSI_ARB_UNDECIDED) << "2 with : 2 without";
  EXPECT_FALSE(nr_csi_arb_applies(&zp));
  uint32_t w = 0, wo = 0, t = 0;
  nr_csi_arb_counts(&zp, &w, &wo, &t);
  EXPECT_EQ(w, 2u);
  EXPECT_EQ(wo, 2u);
  EXPECT_EQ(t, 4u);
}

/* a proven pattern is overturned when the contrary evidence dominates 2:1 */
TEST_F(Arb, KeepIsRevisedByDominantContraryEvidence)
{
  auto zp = pat(4, 8, 0, true, true);
  nr_csi_arb_note(&zp, NR_CSI_ARB_WITH_WINS);
  nr_csi_arb_note(&zp, NR_CSI_ARB_WITH_WINS);
  ASSERT_EQ(nr_csi_arb_verdict(&zp), NR_CSI_ARB_KEEP);
  nr_csi_arb_note(&zp, NR_CSI_ARB_WITHOUT_WINS);
  nr_csi_arb_note(&zp, NR_CSI_ARB_WITHOUT_WINS);
  EXPECT_EQ(nr_csi_arb_verdict(&zp), NR_CSI_ARB_UNDECIDED);
  nr_csi_arb_note(&zp, NR_CSI_ARB_WITHOUT_WINS);
  nr_csi_arb_note(&zp, NR_CSI_ARB_WITHOUT_WINS);
  nr_csi_arb_note(&zp, NR_CSI_ARB_WITHOUT_WINS);
  EXPECT_EQ(nr_csi_arb_verdict(&zp), NR_CSI_ARB_DROP);
  EXPECT_FALSE(nr_csi_arb_applies(&zp));
}

/* trials in which both decodes fail teach nothing; a hole pattern that never wins in NR_CSI_ARB_MAX_TRIALS tries stops being tried */
TEST_F(Arb, UselessPatternStopsCostingTrials)
{
  auto zp = pat(4, 8, 0, true, true);
  for (int i = 0; i < NR_CSI_ARB_MAX_TRIALS - 1; ++i) {
    EXPECT_EQ(nr_csi_arb_note(&zp, NR_CSI_ARB_NO_EVIDENCE), NR_CSI_ARB_UNDECIDED);
    EXPECT_TRUE(nr_csi_arb_trial_add(&zp));
  }
  EXPECT_EQ(nr_csi_arb_note(&zp, NR_CSI_ARB_NO_EVIDENCE), NR_CSI_ARB_DROP);
  EXPECT_FALSE(nr_csi_arb_trial_add(&zp));
  EXPECT_FALSE(nr_csi_arb_applies(&zp));
  /* a sequence pattern is never dropped by failed trials alone */
  auto nzp = pat(4, 4, 2, false, false);
  for (int i = 0; i < 4 * NR_CSI_ARB_MAX_TRIALS; ++i)
    nr_csi_arb_note(&nzp, NR_CSI_ARB_NO_EVIDENCE);
  EXPECT_TRUE(nr_csi_arb_applies(&nzp));
}

/* the verdict follows the geometry, not the slot: same pattern in another PDU copy, different row/symbol/id is another pattern */
TEST_F(Arb, KeyIsTheGeometry)
{
  auto a = pat(4, 8, 0, true, true);
  auto b = a;
  nr_csi_arb_note(&a, NR_CSI_ARB_WITH_WINS);
  nr_csi_arb_note(&b, NR_CSI_ARB_WITH_WINS);
  EXPECT_EQ(nr_csi_arb_verdict(&a), NR_CSI_ARB_KEEP);
  auto c = pat(4, 9, 0, true, true);
  EXPECT_EQ(nr_csi_arb_verdict(&c), NR_CSI_ARB_UNDECIDED);
  auto d = pat(5, 8, 0, true, true);
  EXPECT_EQ(nr_csi_arb_verdict(&d), NR_CSI_ARB_UNDECIDED);
  nr_csi_arb_reset();
  EXPECT_EQ(nr_csi_arb_verdict(&a), NR_CSI_ARB_UNDECIDED);
}

int main(int argc, char **argv) { logInit(); testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS(); }

/* the lattice variant is part of the key: a pattern proven on one phase is not proven on the other */
TEST_F(Arb, LatticeVariantsAreSeparatePatterns)
{
  auto base = pat(4, 8, 0, true, true);
  auto v1 = base;
  v1.last_trs_slot = 1;
  nr_csi_arb_note(&v1, NR_CSI_ARB_WITH_WINS);
  nr_csi_arb_note(&v1, NR_CSI_ARB_WITH_WINS);
  EXPECT_EQ(nr_csi_arb_verdict(&v1), NR_CSI_ARB_KEEP);
  EXPECT_EQ(nr_csi_arb_verdict(&base), NR_CSI_ARB_UNDECIDED);
  EXPECT_FALSE(nr_csi_arb_applies(&base));
  EXPECT_TRUE(nr_csi_arb_applies(&v1));
}
