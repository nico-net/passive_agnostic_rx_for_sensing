#include <cstdint>
#include <cstring>
#include <vector>
#include <gtest/gtest.h>
extern "C" {
#include "nr_pdcch_uss_prune.h"
}

/* Reference values computed independently (python, TS 38.213 10.1 written out from the text). */
TEST(UssHash, YMatchesSpecRecursion)
{
  EXPECT_EQ(nr_uss_hash_Y(17, 0, 39827), 21689u);          // 39827 * 17 mod 65537
  EXPECT_EQ(nr_uss_hash_Y(17, 5, 39829), 40473u);
  EXPECT_EQ(nr_uss_hash_Y(0x4626, 19, 39839), 56203u);
  EXPECT_EQ(nr_uss_hash_Y(65519, 7, 39827), 2599u);
}

static std::vector<int> positions(uint16_t rnti, int slot, int L, int N, uint32_t A, int M)
{
  uint16_t c[NR_USS_PRUNE_MAX_POS];
  const int n = nr_uss_hash_positions(rnti, slot, L, N, A, M, c);
  return std::vector<int>(c, c + n);
}

TEST(UssHash, CandidatePositionsMatchSpecExamples)
{
  EXPECT_EQ(positions(17, 5, 2, 48, 39829, 4), (std::vector<int>{18, 30, 42, 6}));
  EXPECT_EQ(positions(17, 0, 1, 32, 39827, 6), (std::vector<int>{25, 30, 3, 9, 14, 19}));
  EXPECT_EQ(positions(0x4626, 19, 4, 33, 39839, 2), (std::vector<int>{12, 28}));
  EXPECT_EQ(positions(17, 5, 8, 48, 39829, 2), (std::vector<int>{24, 0}));
  // M larger than the number of distinct positions: duplicates are removed (N/L = 1).
  EXPECT_EQ(positions(17, 5, 16, 16, 39829, 2).size(), 1u);
  EXPECT_TRUE(nr_uss_hash_in(17, 5, 30, 2, 48, 39829, 4));
  EXPECT_FALSE(nr_uss_hash_in(17, 5, 20, 2, 48, 39829, 4));
  EXPECT_FALSE(nr_uss_hash_in(17, 5, 30, 2, 48, 39829, 1)); // m = 1 is not in the M = 1 set
}

namespace {
const uint64_t kGeom = 0x1234;
const int kN = 48;
const uint32_t kA = 39829; // p mod 3 = 1
const int kM[5] = {0, 4, 2, 2, 0};

// A UE's DCI in `slot` at its m-th AL candidate; m cycles to exercise all of the fitted M.
void feed(uint16_t rnti, int slot, int L, int m, bool force = false)
{
  const int li = L == 1 ? 0 : L == 2 ? 1 : L == 4 ? 2 : L == 8 ? 3 : 4;
  const int M = kM[li];
  const uint32_t y = nr_uss_hash_Y(rnti, slot, kA);
  const int cce = L * (int)((y + (uint32_t)((m % M) * kN) / (L * M)) % (uint32_t)(kN / L));
  nr_uss_prune_note(kGeom, rnti, slot, cce, L, kN, force);
}
const uint16_t kRntis[6] = {0x4626, 0x4a31, 0x5c07, 0x61d2, 0x7a80, 0x8b4e};

void learn()
{
  nr_uss_prune_reset();
  int k = 0;
  for (int s = 0; s < 20; s++)
    for (int r = 0; r < 6; r++, k++) {
      static const int als[3] = {2, 4, 8};
      feed(kRntis[r], s, als[k % 3], k);
    }
}
} // namespace

TEST(UssPrune, NoPruningBeforeAFitExists)
{
  nr_uss_prune_reset();
  for (int cce = 0; cce < kN; cce += 2)
    EXPECT_TRUE(nr_uss_prune_admit(kGeom, 0x4626, 3, cce, 2, false));
  nr_uss_prune_stats_t st;
  nr_uss_prune_stats(&st);
  EXPECT_EQ(st.pruned, 0u);
}

TEST(UssPrune, FitKeepsExactlyTheHashedSetAndPrunesTheRest)
{
  learn();
  ASSERT_GT(nr_uss_prune_hypotheses(kGeom), 0);
  nr_uss_prune_stats_t st0;
  nr_uss_prune_stats(&st0);
  EXPECT_GT(st0.fits, 0u);
  int kept = 0, total = 0;
  for (int slot : {1, 7, 23}) {
    for (int r = 0; r < 6; r++) {
      for (int L : {2, 4, 8}) {
        const int li = L == 2 ? 1 : L == 4 ? 2 : 3;
        uint16_t pos[NR_USS_PRUNE_MAX_POS];
        const int np = nr_uss_hash_positions(kRntis[r], slot, L, kN, kA, kM[li], pos);
        for (int cce = 0; cce < kN; cce += L) {
          const bool truth = nr_uss_hash_in(kRntis[r], slot, cce, L, kN, kA, kM[li]);
          const bool adm = nr_uss_prune_admit(kGeom, kRntis[r], slot, cce, L, false);
          // never lose a position the true search space contains
          if (truth) EXPECT_TRUE(adm) << "rnti " << r << " slot " << slot << " L " << L << " cce " << cce;
          kept += adm;
          total++;
        }
        (void)np;
      }
    }
  }
  EXPECT_LT(kept * 3, total); // far fewer than all positions are decoded
  nr_uss_prune_stats_t st;
  nr_uss_prune_stats(&st);
  EXPECT_EQ(st.pruned, (uint64_t)(total - kept));
}

TEST(UssPrune, AuditOccasionDecodesEverythingAndCounts)
{
  learn();
  uint64_t admitted = 0;
  for (int cce = 0; cce < kN; cce += 2)
    admitted += nr_uss_prune_admit(kGeom, kRntis[0], 9, cce, 2, true);
  EXPECT_EQ(admitted, (uint64_t)(kN / 2));
  nr_uss_prune_stats_t st;
  nr_uss_prune_stats(&st);
  EXPECT_EQ(st.pruned, 0u);
  EXPECT_GT(st.audit_tasks, 0u);
}

TEST(UssPrune, AuditSlotsAreSparseAndDeterministic)
{
  int n = 0;
  for (uint32_t s = 0; s < 16384; s++) {
    const bool a = nr_uss_prune_audit_slot(s);
    EXPECT_EQ(a, nr_uss_prune_audit_slot(s));
    n += a;
  }
  EXPECT_GT(n, 16384 / 16 * 0.9);
  EXPECT_LT(n, 16384 / 16 * 1.1);
}

TEST(UssPrune, MissDemotesRntiToFullSearchAndRepromotesAfterInSetHits)
{
  learn();
  const int h0 = nr_uss_prune_hypotheses(kGeom);
  ASSERT_GT(h0, 0);
  const uint16_t odd = kRntis[2];
  // a DCI of `odd` at an AL2 position outside its hashed set (and outside any widened fit)
  const int slot = 11;
  int bad = -1;
  for (int cce = 0; cce < kN && bad < 0; cce += 2)
    if (!nr_uss_hash_in(odd, slot, cce, 2, kN, kA, 8) && !nr_uss_hash_in(odd, slot, cce, 2, kN, 39827, 8)
        && !nr_uss_hash_in(odd, slot, cce, 2, kN, 39839, 8))
      bad = cce;
  ASSERT_GE(bad, 0);
  ASSERT_FALSE(nr_uss_prune_allows(kGeom, odd, slot, bad, 2));
  nr_uss_prune_note(kGeom, odd, slot, bad, 2, kN, false);
  nr_uss_prune_stats_t st;
  nr_uss_prune_stats(&st);
  EXPECT_EQ(st.audit_miss, 1u);
  EXPECT_EQ(st.rnti_demotions, 1u);
  // the geometry's fit survives (one odd RNTI must not switch pruning off for the others)
  EXPECT_EQ(nr_uss_prune_hypotheses(kGeom), h0);
  EXPECT_FALSE(nr_uss_prune_allows(kGeom, kRntis[0], slot, bad, 2) && !nr_uss_hash_in(kRntis[0], slot, bad, 2, kN, kA, 4));
  // the demoted RNTI is searched everywhere ...
  for (int cce = 0; cce < kN; cce += 2)
    EXPECT_TRUE(nr_uss_prune_allows(kGeom, odd, 13, cce, 2));
  // ... until it shows two in-set DCIs
  feed(odd, 14, 2, 0);
  nr_uss_prune_stats(&st);
  EXPECT_EQ(st.rnti_repromotions, 0u);
  feed(odd, 15, 4, 1);
  nr_uss_prune_stats(&st);
  EXPECT_EQ(st.rnti_repromotions, 1u);
  int kept = 0;
  for (int cce = 0; cce < kN; cce += 2)
    kept += nr_uss_prune_allows(kGeom, odd, 13, cce, 2);
  EXPECT_LT(kept, kN / 2);
}

TEST(UssPrune, RunOfMissesResetsTheFit)
{
  learn();
  ASSERT_GT(nr_uss_prune_hypotheses(kGeom), 0);
  int misses = 0;
  // a cell whose search space changed: DCIs now follow a different multiplier (p mod 3 = 0 hash)
  for (int s = 30; s < 200 && misses < 40; s++) {
    for (int r = 0; r < 6; r++) {
      const uint32_t y = nr_uss_hash_Y(kRntis[r], s, 39827);
      const int cce = 2 * (int)(y % (kN / 2));
      if (!nr_uss_hash_in(kRntis[r], s, cce, 2, kN, kA, 8)) {
        nr_uss_prune_note(kGeom, kRntis[r], s, cce, 2, kN, false);
        misses++;
      }
    }
    nr_uss_prune_stats_t st;
    nr_uss_prune_stats(&st);
    if (st.geom_resets) break;
  }
  nr_uss_prune_stats_t st;
  nr_uss_prune_stats(&st);
  EXPECT_GE(st.geom_resets, 1u);
  EXPECT_GE(st.audit_miss, 8u);
}

TEST(UssPrune, ForcedUlSampleThatNoHypothesisSurvivesIsTakenBack)
{
  learn();
  const int h0 = nr_uss_prune_hypotheses(kGeom);
  const int slot = 12;
  int bad = -1;
  for (int cce = 0; cce < kN && bad < 0; cce += 2)
    if (!nr_uss_hash_in(kRntis[1], slot, cce, 2, kN, kA, 8) && !nr_uss_hash_in(kRntis[1], slot, cce, 2, kN, 39827, 8)
        && !nr_uss_hash_in(kRntis[1], slot, cce, 2, kN, 39839, 8))
      bad = cce;
  ASSERT_GE(bad, 0);
  EXPECT_FALSE(nr_uss_prune_note(kGeom, kRntis[1], slot, bad, 2, kN, true));
  EXPECT_EQ(nr_uss_prune_hypotheses(kGeom), h0);
}
