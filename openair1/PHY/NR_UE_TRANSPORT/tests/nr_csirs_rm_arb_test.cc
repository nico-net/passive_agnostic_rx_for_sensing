// fix1031/rm: CSI-RS / TRS rate-matching candidate family and its CRC arbitration (nr_csirs_rm_arb.h).
#include <gtest/gtest.h>
#include <cstring>
#include "nr_csirs_rm_arb.h"
#include "nr_re_pattern.h"

namespace {
typedef fapi_nr_dl_config_csirs_pdu_rel15_t pdu_t;

pdu_t trs(int id, int start, int n, int l0 = 5)
{
  pdu_t p;
  memset(&p, 0, sizeof(p));
  p.csi_type = 1;
  p.row = 1;
  p.freq_domain = 1;
  p.symb_l0 = (uint8_t)l0;
  p.freq_density = 3;
  p.scramb_id = (uint16_t)id;
  p.start_rb = (uint16_t)start;
  p.nr_of_rbs = (uint16_t)n;
  return p;
}
pdu_t foot(int sym, int sc, int start, int n)
{
  pdu_t p;
  nr_re_pattern_make(&p, (uint32_t)sym, (uint32_t)sc, 3, start, n);
  return p;
}
const uint16_t kDmrs = (1u << 2) | (1u << 11); // symbols 2 and 11

// The "air": a grant decodes only when the PDSCH rate-matches around the TRS (sel covers it) and erases nothing that carries data. The truth here is
// "the TRS id=7 at 0:217 is a real resource of this cell"; erasing it (what the old rule does for a foreign ID) loses its REs.
bool crc_passes_for_trs(const nr_rm_cand_t &c, int start, int n)
{
  bool covered = false;
  for (int i = 0; i < c.n_sel; i++)
    covered |= c.sel[i].start_rb <= start && c.sel[i].start_rb + c.sel[i].nr_of_rbs >= start + n;
  return covered && c.n_er == 0;
}

// the production flow of rm_arb_step(), against a CRC oracle
template <typename Oracle>
int decode_grant(uint16_t rnti, const nr_rm_cands_t &cs, uint8_t rm, uint8_t zp, Oracle ok, bool *rescued = nullptr)
{
  const int first = nr_rm_arb_first(rnti, rm, zp, &cs);
  const bool pass = ok(cs.c[first]);
  nr_rm_arb_note(rnti, rm, zp, &cs, first, -1, pass, true);
  if (rescued) *rescued = false;
  if (pass)
    return first;
  unsigned tried = 1u << first;
  for (int k; (k = nr_rm_arb_next(rnti, rm, zp, &cs, tried)) >= 0;) {
    tried |= 1u << k;
    if (ok(cs.c[k])) {
      nr_rm_arb_note(rnti, rm, zp, &cs, k, first, true, true);
      if (rescued) *rescued = true;
      return k;
    }
  }
  return -1;
}
} // namespace

class RmArb : public ::testing::Test {
 protected:
  void SetUp() override { nr_rm_arb_reset(); }
};

TEST_F(RmArb, FragmentsOfOneResourceMergeIntoTheirHull)
{
  pdu_t p[3] = {trs(7, 0, 52), trs(7, 128, 52), trs(7, 64, 116)}; // the PCI-415 footprint fragments (audit D5)
  uint8_t m[3];
  EXPECT_EQ(nr_rm_consolidate(p, 3, m), 1);
  EXPECT_EQ(p[0].start_rb, 0);
  EXPECT_EQ(p[0].nr_of_rbs, 180);
  EXPECT_EQ(m[0], 3);
}
TEST_F(RmArb, DifferentIdsAndFarRangesAreDifferentResources)
{
  pdu_t a[2] = {trs(7, 0, 52), trs(9, 52, 52)};
  EXPECT_EQ(nr_rm_consolidate(a, 2, nullptr), 2);
  pdu_t b[2] = {trs(7, 0, 52), trs(7, 120, 52)}; // gap 68 >= width 52
  EXPECT_EQ(nr_rm_consolidate(b, 2, nullptr), 2);
  pdu_t c[2] = {trs(7, 0, 52), trs(7, 52, 52)}; // touching
  EXPECT_EQ(nr_rm_consolidate(c, 2, nullptr), 1);
  EXPECT_EQ(c[0].nr_of_rbs, 104);
}
TEST_F(RmArb, NoCsiRsMeansOneCandidateAndNoArbitration)
{
  nr_rm_cands_t cs;
  nr_rm_build(nullptr, 0, nullptr, 0, nullptr, 0, 3, kDmrs, 0, 273, &cs);
  EXPECT_EQ(cs.n, 1);
  EXPECT_EQ(cs.cls, 0u);
  EXPECT_EQ(cs.c[0].n_sel, 0);
  EXPECT_EQ(cs.c[0].n_er, 0);
  EXPECT_EQ(nr_rm_arb_first(0x4601, 0, 0, &cs), 0);
}
TEST_F(RmArb, OwnRuleIsCandidateZeroAndMatchesTheFormerSelection)
{
  const pdu_t conf[2] = {trs(415, 0, 52), trs(748, 64, 52)};
  nr_rm_cands_t cs;
  nr_rm_build(conf, 2, conf, 2, nullptr, 0, 415, kDmrs, 0, 273, &cs);
  ASSERT_GE(cs.n, 2);
  EXPECT_EQ(cs.c[0].kind, NR_RM_OWN);
  EXPECT_EQ(cs.c[0].n_sel, 1); // own 415 (748 sits in another RB third: erased, the old behaviour)
  EXPECT_EQ(cs.c[0].sel[0].scramb_id, 415);
  EXPECT_EQ(cs.c[0].n_er, 1);
  EXPECT_EQ(cs.c[0].er[0].scramb_id, 748);
}
TEST_F(RmArb, TrsWithAnotherScramblingIdThanTheDmrsIsAdoptedByCrc)
{
  const pdu_t conf[1] = {trs(7, 0, 217)};
  nr_rm_cands_t cs;
  nr_rm_build(conf, 1, conf, 1, nullptr, 0, /* DM-RS scrambling ID */ 3, kDmrs, 0, 273, &cs);
  ASSERT_GE(cs.n, 2);
  // old code: candidate 0 only. It erases the foreign-ID TRS and fails.
  EXPECT_EQ(cs.c[0].n_sel, 0);
  EXPECT_EQ(cs.c[0].n_er, 1);
  auto air = [](const nr_rm_cand_t &c) { return crc_passes_for_trs(c, 0, 217); };
  EXPECT_FALSE(air(cs.c[0]));
  bool rescued = false;
  int k = decode_grant(0x4601, cs, 0, 0, air, &rescued);
  ASSERT_GE(k, 0);
  EXPECT_EQ(cs.c[k].kind, NR_RM_ALL_CONF);
  EXPECT_TRUE(rescued);
  // a second pass adopts it for the RNTI: the next grant needs no alternate
  k = decode_grant(0x4601, cs, 0, 0, air, &rescued);
  EXPECT_TRUE(rescued);
  EXPECT_EQ(nr_rm_arb_adopted(0x4601, cs.cls, 0, 0), NR_RM_ALL_CONF);
  const int first = nr_rm_arb_first(0x4601, 0, 0, &cs);
  EXPECT_EQ(cs.c[first].kind, NR_RM_ALL_CONF);
  k = decode_grant(0x4601, cs, 0, 0, air, &rescued);
  EXPECT_FALSE(rescued);
  EXPECT_GE(k, 0);
  // a new RNTI inherits the cell's decision once the cell has 3 passes
  EXPECT_EQ(cs.c[nr_rm_arb_first(0x4602, 0, 0, &cs)].kind, NR_RM_ALL_CONF);
}
TEST_F(RmArb, OwnIdRuleStaysWhenItWorks)
{
  const pdu_t conf[2] = {trs(415, 0, 52), trs(748, 64, 52)};
  nr_rm_cands_t cs;
  nr_rm_build(conf, 2, conf, 2, nullptr, 0, 415, kDmrs, 0, 273, &cs);
  auto air = [](const nr_rm_cand_t &c) { return c.kind == NR_RM_OWN; };
  for (int i = 0; i < 6; i++) {
    bool rescued = true;
    EXPECT_EQ(decode_grant(0x4601, cs, 0, 0, air, &rescued), 0);
    EXPECT_FALSE(rescued);
  }
  EXPECT_EQ(nr_rm_arb_adopted(0x4601, cs.cls, 0, 0), NR_RM_OWN);
}
TEST_F(RmArb, FragmentedFootprintsBecomeOneWidebandCandidate)
{
  // discovery saw the TRS only as footprints 0:52, 128:52, 64:116 (symbol 5 | 9, subcarrier 0)
  const pdu_t fp[3] = {foot(0x220, 1, 0, 52), foot(0x220, 1, 128, 52), foot(0x220, 1, 64, 116)};
  nr_rm_cands_t cs;
  nr_rm_build(nullptr, 0, nullptr, 0, fp, 3, 3, kDmrs, 0, 273, &cs);
  int kf = -1, kw = -1;
  for (int i = 0; i < cs.n; i++) {
    if (cs.c[i].kind == NR_RM_ALL_FOOT) kf = i;
    if (cs.c[i].kind == NR_RM_ALL_WIDE) kw = i;
  }
  ASSERT_GE(kf, 0);
  ASSERT_GE(kw, 0);
  EXPECT_EQ(cs.c[kf].n_sel, 1);
  EXPECT_EQ(cs.c[kf].sel[0].start_rb, 0);
  EXPECT_EQ(cs.c[kf].sel[0].nr_of_rbs, 180);
  EXPECT_EQ(cs.c[kw].sel[0].start_rb, 0);
  EXPECT_EQ(cs.c[kw].sel[0].nr_of_rbs, 273);
  // the truth is a full-band TRS: only the widened candidate decodes
  auto air = [](const nr_rm_cand_t &c) { return crc_passes_for_trs(c, 0, 217); };
  bool rescued = false;
  const int k = decode_grant(0x4601, cs, 0, 0, air, &rescued);
  ASSERT_GE(k, 0);
  EXPECT_EQ(cs.c[k].kind, NR_RM_ALL_WIDE);
}
TEST_F(RmArb, FootprintLosesItsDmrsSymbolsAndClashingResourcesAreDropped)
{
  const pdu_t fp[1] = {foot((1 << 2) | (1 << 5), 1, 0, 100)}; // DM-RS symbol 2 and a TRS symbol 5
  const pdu_t clash = trs(7, 0, 217, 2);                          // TRS on a DM-RS symbol: the standard forbids it
  const pdu_t ok = trs(7, 0, 217, 5);
  const pdu_t all[2] = {clash, ok};
  nr_rm_cands_t cs;
  nr_rm_build(all, 2, all, 2, fp, 1, 3, kDmrs, 0, 273, &cs);
  for (int i = 0; i < cs.n; i++)
    for (int j = 0; j < cs.c[i].n_sel; j++) {
      EXPECT_FALSE(nr_rm_dmrs_clash(&cs.c[i].sel[j], kDmrs) && !nr_re_pattern_is(&cs.c[i].sel[j]));
      if (nr_re_pattern_is(&cs.c[i].sel[j]))
        EXPECT_EQ(nr_re_pattern_sym(&cs.c[i].sel[j]) & kDmrs, 0u);
    }
}
TEST_F(RmArb, TheDciIndicatorValueKeysTheDecision)
{
  const pdu_t conf[1] = {trs(7, 0, 217)};
  nr_rm_cands_t cs;
  nr_rm_build(conf, 1, conf, 1, nullptr, 0, 3, kDmrs, 0, 273, &cs);
  auto with = [](const nr_rm_cand_t &c) { return crc_passes_for_trs(c, 0, 217); };
  auto without = [](const nr_rm_cand_t &c) { return c.kind == NR_RM_NONE; }; // indicator 1: the REs are data
  for (int i = 0; i < 4; i++) {
    decode_grant(0x4601, cs, /* rm_ind */ 0, 0, with);
    decode_grant(0x4601, cs, /* rm_ind */ 1, 0, without);
  }
  EXPECT_EQ(nr_rm_arb_adopted(0x4601, cs.cls, 0, 0), NR_RM_ALL_CONF);
  EXPECT_EQ(nr_rm_arb_adopted(0x4601, cs.cls, 1, 0), NR_RM_NONE);
}
TEST_F(RmArb, AdoptedCandidateIsRetestedAfterAFailureStreak)
{
  const pdu_t conf[1] = {trs(7, 0, 217)};
  nr_rm_cands_t cs;
  nr_rm_build(conf, 1, conf, 1, nullptr, 0, 3, kDmrs, 0, 273, &cs);
  auto conf_ok = [](const nr_rm_cand_t &c) { return crc_passes_for_trs(c, 0, 217); };
  for (int i = 0; i < 3; i++)
    decode_grant(0x4601, cs, 0, 0, conf_ok);
  ASSERT_EQ(nr_rm_arb_adopted(0x4601, cs.cls, 0, 0), NR_RM_ALL_CONF);
  auto none_ok = [](const nr_rm_cand_t &c) { return c.kind == NR_RM_NONE; }; // the network reconfigured
  for (int i = 0; i < 3; i++)
    decode_grant(0x4601, cs, 0, 0, none_ok);
  EXPECT_NE(nr_rm_arb_adopted(0x4601, cs.cls, 0, 0), NR_RM_ALL_CONF);
  for (int i = 0; i < 4; i++)
    decode_grant(0x4601, cs, 0, 0, none_ok);
  EXPECT_EQ(nr_rm_arb_adopted(0x4601, cs.cls, 0, 0), NR_RM_NONE);
}
TEST_F(RmArb, IdenticalCandidatesAreNotRepeated)
{
  // the own rule already rate-matches everything: ALL_CONF equals OWN and must not appear
  const pdu_t conf[1] = {trs(3, 0, 217)};
  nr_rm_cands_t cs;
  nr_rm_build(conf, 1, conf, 1, nullptr, 0, 3, kDmrs, 0, 273, &cs);
  EXPECT_EQ(cs.n, 2);
  EXPECT_EQ(cs.c[0].kind, NR_RM_OWN);
  EXPECT_EQ(cs.c[1].kind, NR_RM_NONE);
}
