// SLOTPHASE (nr_pdsch_passive_slotphase.c): F1 cell-level phase evidence and the CRC arbitration of a provisional rate-match set.
// Simulated cell: strong-DM-RS first transmissions whose CRC passes 95 % except at the failing phases, where it passes only under the variant
// the test names (a stand-in for the right CSI-RS RE set); the module sees nothing but slots, tokens and CRC bits.
#include <cstdlib>
#include <cstdio>
#include <random>
#include <vector>
#include <cmath>
#include <cstring>
#include <gtest/gtest.h>
extern "C" {
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
#include "nr_pdsch_passive_slotphase.h"
#include "nr_re_pattern.h"
}
extern "C" configmodule_interface_t *uniqCfg = nullptr;
extern "C" void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
  if (s) fprintf(stderr, "%s:%d %s() Exiting: %s\n", file, line, function, s);
  if (assert) abort(); else exit(EXIT_SUCCESS);
}

namespace {
// the legacy shape of the call: seeds only
nr_slotphase_choice_t P(uint32_t slot, uint32_t gen, uint32_t token, const uint32_t *seeds, int n_seeds, bool has_conf)
{
  nr_slotphase_in_t in = {};
  in.seed_ids = seeds;
  in.n_seeds = n_seeds;
  in.has_conf = has_conf;
  return nr_slotphase_plan(slot, gen, token, &in);
}
struct Sim {
  std::mt19937 rng{7};
  uint32_t token = 0;
  uint32_t gen = 1;
  int good_period = 40, good_a = 8, good_b = 18; // failing phases abs % 40 in {8, 18}
  // variant that makes a failing slot pass: kind (NR_SP_*) and, for NR_SP_SEED, the seed id; -1 = none does
  int good_kind = NR_SP_UNION;
  uint32_t good_seed = 0;
  uint32_t seeds[2] = {11, 12};
  int n_seeds = 2;
  bool has_conf = false;
  uint32_t foots[3] = {21, 22, 23};
  int n_foot = 0;
  uint32_t good_foot = 0;
  int good_trim = 0;
  int nsym = 0;
  bool trim_ok = false;
  int tried[8] = {0};
  // one strong grant at slot s: plan, "decode", note. Returns the choice.
  nr_slotphase_choice_t Grant(uint32_t s)
  {
    nr_slotphase_in_t in = {};
    in.seed_ids = seeds;
    in.n_seeds = n_seeds;
    in.foot_ids = foots;
    in.n_foot = n_foot;
    in.has_conf = has_conf;
    in.nsym = nsym;
    in.trim_ok = trim_ok;
    const nr_slotphase_choice_t ch = nr_slotphase_plan(s, gen, ++token, &in);
    const bool bad = (s % good_period) == (uint32_t)good_a || (s % good_period) == (uint32_t)good_b;
    bool ok;
    if (!bad) ok = (rng() % 100) < 95;
    else {
      const bool right = ch.kind == good_kind && (good_kind != NR_SP_SEED || seeds[ch.seed] == good_seed)
                         && (good_kind != NR_SP_FOOT || foots[ch.foot] == good_foot) && (good_kind != NR_SP_TRIM || ch.trim == good_trim);
      ok = right && (rng() % 100) < 90;
    }
    nr_slotphase_note(s, gen, token, ok);
    tried[ch.kind]++;
    return ch;
  }
  void Run(uint32_t from, uint32_t slots)
  {
    for (uint32_t s = from; s < from + slots; s++) Grant(s);
  }
};
} // namespace

TEST(SlotPhase, WilsonBounds)
{
  EXPECT_NEAR(nr_slotphase_wilson_hi(0, 30), 3.8416 / (30 + 3.8416), 1e-3);
  EXPECT_DOUBLE_EQ(nr_slotphase_wilson_lo(0, 30), 0.0);
  EXPECT_DOUBLE_EQ(nr_slotphase_wilson_hi(0, 0), 1.0);
  EXPECT_GT(nr_slotphase_wilson_lo(3, 3), 0.43);
  EXPECT_LT(nr_slotphase_wilson_lo(95, 100), 0.95);
  EXPECT_GT(nr_slotphase_wilson_hi(95, 100), 0.95);
}

TEST(SlotPhase, StrongNeedsSixDbOverRequirementAndFirstTransmission)
{
  // mcs 0, table 0: QPSK R=120/1024 -> required = 10 log10(2^(2*120/1024) - 1) + 1.5 = -6.0 dB; strong from about 0 dB
  EXPECT_TRUE(nr_slotphase_strong(5.0, 0, 0, 1, 0, true, false));
  EXPECT_FALSE(nr_slotphase_strong(-3.0, 0, 0, 1, 0, true, false)); // margin below 6 dB
  EXPECT_FALSE(nr_slotphase_strong(20.0, 0, 0, 1, 1, true, false)); // retransmission
  EXPECT_FALSE(nr_slotphase_strong(20.0, 0, 0, 3, 0, true, false)); // 3 layers: layer separation, not a slot property
  EXPECT_FALSE(nr_slotphase_strong(20.0, 0, 0, 1, 0, false, false)); // SI/P/RA
  EXPECT_FALSE(nr_slotphase_strong(20.0, 0, 0, 1, 0, true, true));   // layout probe
  EXPECT_FALSE(nr_slotphase_strong(-99.0, 0, 0, 1, 0, true, false)); // no estimate
  EXPECT_FALSE(nr_slotphase_strong(5.0, 31, 0, 1, 0, true, false));  // reserved MCS
  EXPECT_FALSE(nr_slotphase_strong(5.0, 0, 0, 0, 0, true, false));
}

TEST(SlotPhase, HealthyCellFlagsNothing)
{
  nr_slotphase_reset();
  Sim sim;
  sim.good_a = sim.good_b = 1000; // no failing phase
  sim.Run(0, 4000);
  EXPECT_FALSE(nr_slotphase_active());
  EXPECT_FALSE(nr_slotphase_covers(8));
}

TEST(SlotPhase, FlagsTheFailingPhasesAtTheSmallestPeriod)
{
  nr_slotphase_reset();
  Sim sim;
  sim.n_seeds = 0; // flag only: nothing to arbitrate
  sim.Run(0, 4000);
  ASSERT_TRUE(nr_slotphase_active());
  int st = -1, ad = -1;
  EXPECT_EQ(nr_slotphase_query(8, &st, &ad), 40);
  EXPECT_EQ(nr_slotphase_query(48, &st, &ad), 40);
  EXPECT_EQ(nr_slotphase_query(18, &st, &ad), 40);
  EXPECT_EQ(nr_slotphase_query(28, &st, &ad), 0);
  EXPECT_EQ(nr_slotphase_query(0, &st, &ad), 0);
  EXPECT_EQ(nr_slotphase_query(38, &st, &ad), 0);
  // no seed in the slot: the decode stays as always
  const nr_slotphase_choice_t ch = P(8, sim.gen, 99999, nullptr, 0, false);
  EXPECT_EQ(ch.kind, NR_SP_CTRL);
  EXPECT_GT(ch.ent, 0); // tagged with its entry, so its outcome counts as the phase's control
}

TEST(SlotPhase, AdoptsTheUnionWhenItPassesAndInheritsIt)
{
  nr_slotphase_reset();
  Sim sim;
  sim.good_kind = NR_SP_UNION;
  sim.Run(0, 12000);
  int st = -1, ad = -1;
  ASSERT_EQ(nr_slotphase_query(8, &st, &ad), 40);
  EXPECT_EQ(st, NR_SP_ADOPTED);
  EXPECT_EQ(ad, NR_SP_UNION);
  // the adopted variant is what every later grant at the phase gets, whichever slot of the phase (and at the other flagged phase)
  EXPECT_EQ(P(88, sim.gen, 1, sim.seeds, 2, false).kind, NR_SP_UNION);
  EXPECT_EQ(P(18, sim.gen, 2, sim.seeds, 2, false).kind, NR_SP_UNION);
  EXPECT_EQ(P(10, sim.gen, 3, sim.seeds, 2, false).kind, NR_SP_CTRL); // an unflagged slot is untouched
  EXPECT_EQ(P(10, sim.gen, 3, sim.seeds, 2, false).ent, 0);
  // slot without seeds: nothing to apply
  EXPECT_EQ(P(8, sim.gen, 4, nullptr, 0, false).kind, NR_SP_CTRL);
}

TEST(SlotPhase, AdoptsASingleSeedByIdNotByPosition)
{
  nr_slotphase_reset();
  Sim sim;
  sim.good_kind = NR_SP_SEED;
  sim.good_seed = 12;
  sim.Run(0, 20000);
  int st = -1, ad = -1;
  ASSERT_EQ(nr_slotphase_query(8, &st, &ad), 40);
  ASSERT_EQ(st, NR_SP_ADOPTED);
  // the seed list of another slot holds the good seed first: the variant follows the id
  const uint32_t other[2] = {12, 31};
  nr_slotphase_choice_t ch = P(48, sim.gen, 5, other, 2, false);
  EXPECT_EQ(ch.kind, NR_SP_SEED);
  EXPECT_EQ(ch.seed, 0);
  // and it vanishes where that seed does not occur
  const uint32_t none[1] = {31};
  EXPECT_EQ(P(48, sim.gen, 6, none, 1, false).kind, NR_SP_CTRL);
}

TEST(SlotPhase, ReplaceIsOnlyOfferedWhereThereIsAConfirmedSet)
{
  nr_slotphase_reset();
  Sim sim;
  sim.good_kind = NR_SP_REPLACE;
  sim.has_conf = true;
  sim.Run(0, 12000);
  int st = -1, ad = -1;
  ASSERT_EQ(nr_slotphase_query(8, &st, &ad), 40);
  EXPECT_EQ(st, NR_SP_ADOPTED);
  EXPECT_EQ(ad, NR_SP_REPLACE);
  EXPECT_GT(sim.tried[NR_SP_REPLACE], 0);
}

TEST(SlotPhase, DropsWhatNeverPassesAndStopsTrying)
{
  nr_slotphase_reset();
  Sim sim;
  sim.good_kind = 99; // no provisional set explains the phase (e.g. the cause is not a rate match)
  sim.Run(0, 6000);
  int st = -1, ad = -1;
  ASSERT_EQ(nr_slotphase_query(8, &st, &ad), 40);
  EXPECT_EQ(st, NR_SP_EXPLORE);
  // all variants dead: the grants decode as always again (the bounded cost of the test)
  for (uint32_t k = 0; k < 20; k++) EXPECT_EQ(P(8 + 40 * k, sim.gen, 7000 + k, sim.seeds, 2, false).kind, NR_SP_CTRL);
  const int before = sim.tried[NR_SP_UNION] + sim.tried[NR_SP_SEED] + sim.tried[NR_SP_REPLACE];
  sim.Run(6000, 4000);
  EXPECT_EQ(before, sim.tried[NR_SP_UNION] + sim.tried[NR_SP_SEED] + sim.tried[NR_SP_REPLACE]);
  // bounded trial cost: 16 trials per variant (union + 2 seeds) per flagged phase (two phases)
  EXPECT_LE(before, 2 * 3 * NR_SLOTPHASE_DROP_N);
}

TEST(SlotPhase, NewCsirsGenerationReopensAnUndecidedPhaseButKeepsAnAdoptedOne)
{
  nr_slotphase_reset();
  Sim sim;
  sim.good_kind = 99;
  sim.Run(0, 6000);
  int st = -1, ad = -1;
  ASSERT_EQ(nr_slotphase_query(8, &st, &ad), 40);
  ASSERT_EQ(P(8, sim.gen, 1, sim.seeds, 2, false).kind, NR_SP_CTRL);
  sim.gen++; // a seed was added or the confirmed set changed
  bool tried = false;
  for (uint32_t k = 0; k < 8; k++) tried |= P(8 + 40 * k, sim.gen, 100 + k, sim.seeds, 2, false).kind != NR_SP_CTRL;
  EXPECT_TRUE(tried);
}

TEST(SlotPhase, AdoptedVariantThatStopsPassingIsRevoked)
{
  nr_slotphase_reset();
  Sim sim;
  sim.good_kind = NR_SP_UNION;
  sim.Run(0, 12000);
  int st = -1, ad = -1;
  ASSERT_EQ(nr_slotphase_query(8, &st, &ad), 40);
  ASSERT_EQ(st, NR_SP_ADOPTED);
  sim.good_kind = 99; // the channel/config changed: the adopted set no longer helps
  sim.Run(12000, 12000);
  nr_slotphase_query(8, &st, &ad);
  EXPECT_EQ(st, NR_SP_EXPLORE);
}

TEST(SlotPhase, FootprintVariantsAreKeyedByIdAndAdopted)
{
  nr_slotphase_reset();
  Sim sim;
  sim.n_seeds = 0;
  sim.n_foot = 3;
  sim.good_kind = NR_SP_FOOT;
  sim.good_foot = 22;
  sim.Run(0, 30000);
  int st = -1, ad = -1;
  ASSERT_EQ(nr_slotphase_query(8, &st, &ad), 40);
  ASSERT_EQ(st, NR_SP_ADOPTED);
  EXPECT_GT(sim.tried[NR_SP_FUNION], 0);
  // the footprint list of another slot holds it last: the variant follows the id
  nr_slotphase_in_t in = {};
  const uint32_t f2[2] = {31, 22};
  in.foot_ids = f2;
  in.n_foot = 2;
  nr_slotphase_choice_t ch = nr_slotphase_plan(48, sim.gen, 5, &in);
  EXPECT_EQ(ch.kind, NR_SP_FOOT);
  EXPECT_EQ(ch.foot, 1);
  const uint32_t f1[1] = {31};
  in.foot_ids = f1;
  in.n_foot = 1;
  EXPECT_EQ(nr_slotphase_plan(48, sim.gen, 6, &in).kind, NR_SP_CTRL);
}

TEST(SlotPhase, FootprintUnionIsAdoptedAndOnlyConfirmedUnionWhereThereIsAConfirmedSet)
{
  nr_slotphase_reset();
  Sim sim;
  sim.n_seeds = 0;
  sim.n_foot = 3;
  sim.has_conf = true;
  sim.good_kind = NR_SP_FUNION_CONF;
  sim.Run(0, 30000);
  int st = -1, ad = -1;
  ASSERT_EQ(nr_slotphase_query(8, &st, &ad), 40);
  EXPECT_EQ(st, NR_SP_ADOPTED);
  EXPECT_GT(sim.tried[NR_SP_FUNION_CONF], 0);
  nr_slotphase_in_t in = {};
  const uint32_t f[2] = {21, 22};
  in.foot_ids = f;
  in.n_foot = 2;
  EXPECT_EQ(nr_slotphase_plan(88, sim.gen, 9, &in).kind, NR_SP_FUNION_CONF);
  in.n_foot = 0;
  EXPECT_EQ(nr_slotphase_plan(88, sim.gen, 10, &in).kind, NR_SP_CTRL);
}

TEST(SlotPhase, EndTrimIsOfferedWhereTheSlotMayBePartialOrNothingElseExplains)
{
  nr_slotphase_reset();
  Sim sim;
  sim.n_seeds = 0; // nothing else explains the phase: trims are offered by themselves
  sim.nsym = 11;
  sim.good_kind = NR_SP_TRIM;
  sim.good_trim = 2;
  sim.Run(0, 30000);
  int st = -1, ad = -1;
  ASSERT_EQ(nr_slotphase_query(8, &st, &ad), 40);
  ASSERT_EQ(st, NR_SP_ADOPTED);
  EXPECT_GT(sim.tried[NR_SP_TRIM], 0);
  nr_slotphase_in_t in = {};
  in.nsym = 13;
  nr_slotphase_choice_t ch = nr_slotphase_plan(88, sim.gen, 11, &in);
  EXPECT_EQ(ch.kind, NR_SP_TRIM);
  EXPECT_EQ(ch.trim, 2);
  in.nsym = 4; // 4 - 2 < 3 symbols: not expressible
  EXPECT_EQ(nr_slotphase_plan(88, sim.gen, 12, &in).kind, NR_SP_CTRL);
  in.nsym = 0; // length unknown
  EXPECT_EQ(nr_slotphase_plan(88, sim.gen, 13, &in).kind, NR_SP_CTRL);
}

TEST(SlotPhase, NoTrimWhenASeedExplainsAndTheSlotIsAFullDlSlot)
{
  nr_slotphase_reset();
  Sim sim;
  sim.nsym = 11;
  sim.trim_ok = false; // TDD known: full DL slot; seeds exist
  sim.good_kind = 99;
  sim.Run(0, 6000);
  EXPECT_EQ(sim.tried[NR_SP_TRIM], 0);
}

TEST(SlotPhase, ResetForgetsTheCell)
{
  nr_slotphase_reset();
  Sim sim;
  sim.good_kind = NR_SP_UNION;
  sim.Run(0, 12000);
  ASSERT_TRUE(nr_slotphase_active());
  nr_slotphase_reset();
  EXPECT_FALSE(nr_slotphase_active());
  EXPECT_FALSE(nr_slotphase_covers(8));
  int st = -1, ad = -1;
  EXPECT_EQ(nr_slotphase_query(8, &st, &ad), 0);
}


// ---------------------------------------------------------------------------------------------------------------------------------
// RESID estimator (nr_slotphase_resid.c): a synthetic grant with a KNOWN blocked RE set must give exactly that footprint; clean grants none.
namespace {
struct Blk {
  uint32_t sym, sc; // symbol mask, 12-bit sc mask
  int dens;         // 0 / 1 = even / odd CRB, 2 = every CRB
  int rb0, nrb;     // CRB range
};
struct GTab {
  float e[NR_SPR_NSYM][NR_SPR_N];
  uint32_t n[NR_SPR_NSYM][NR_SPR_N];
};
static bool is_blocked(const std::vector<Blk> &blk, int s, int crb, int sc)
{
  for (const Blk &b : blk)
    if ((b.sym >> s & 1) && (b.sc >> sc & 1) && crb >= b.rb0 && crb < b.rb0 + b.nrb && (b.dens == 2 || (crb & 1) == b.dens)) return true;
  return false;
}
// One decoded grant as the GPU demod would report it: layer-0 lattice error per RE ~ mean * Exp(1) (mean = the grant's own SNR), x40 on blocked REs.
// DM-RS on symbols 2 and 11 (even subcarriers), PDSCH from symbol s0 for nsym symbols.
static void MakeGrant(GTab &g, std::mt19937 &rng, int rb0, int nrb, int s0, int nsym, const std::vector<Blk> &blk)
{
  memset(&g, 0, sizeof(g));
  std::uniform_real_distribution<double> u(0.0, 1.0);
  const double mean = 0.01 * std::pow(10.0, 1.5 * u(rng)); // 0.01 .. 0.3 (5-20 dB)
  for (int s = s0; s < s0 + nsym && s < 14; s++)
    for (int crb = rb0; crb < rb0 + nrb; crb++)
      for (int sc = 0; sc < 12; sc++) {
        if ((s == 2 || s == 11) && sc % 2 == 0) continue; // DM-RS RE
        const double e = mean * (is_blocked(blk, s, crb, sc) ? 40.0 : 1.0) * -std::log(1.0 - u(rng));
        const int key = sc + 12 * (crb & 3), bk = std::min(crb >> 3, NR_SPR_BKT - 1);
        g.e[s][key] += (float)e;
        g.n[s][key]++;
        g.e[s][NR_SPR_KEYS + bk] += (float)e;
        g.n[s][NR_SPR_KEYS + bk]++;
      }
}
static void FillTabs(std::mt19937 &rng, nr_spr_tab_t *fail, nr_spr_tab_t *ra, nr_spr_tab_t *rb, int nf, int nr, const std::vector<Blk> &blk, int rb_max = 217)
{
  GTab g;
  for (int i = 0; i < nf + nr; i++) {
    const int nrb = 20 + (int)(rng() % (rb_max - 20)), rb0 = (int)(rng() % (rb_max - nrb + 1)); // >= 20 RBs: a key bin then holds >= 4 REs per symbol
    const bool f = i < nf;
    MakeGrant(g, rng, rb0, nrb, 1 + (int)(rng() % 2), 11 + (int)(rng() % 2), f ? blk : std::vector<Blk>());
    nr_spr_tab_t *t = f ? fail : (i % 2 ? ra : rb);
    ASSERT_TRUE(nr_spr_add(t, g.e, g.n));
  }
}
} // namespace

TEST(SlotPhaseResid, NormalQuantile)
{
  EXPECT_NEAR(nr_spr_norm_quantile(0.5), 0.0, 1e-9);
  EXPECT_NEAR(nr_spr_norm_quantile(0.975), 1.959964, 1e-5);
  EXPECT_NEAR(nr_spr_norm_quantile(1.0 - 2.5e-6), 4.5648, 1e-3);
}

TEST(SlotPhaseResid, ExactFootprintOfAWholeBandMultiPortResource)
{
  // the 923 hypothesis: symbols 8, 9, 12, 13, subcarriers 4..11, every RB
  for (int seed = 1; seed <= 5; seed++) {
    std::mt19937 rng(seed);
    auto *f = new nr_spr_tab_t(), *a = new nr_spr_tab_t(), *b = new nr_spr_tab_t();
    FillTabs(rng, f, a, b, 64, 64, {{0x3300, 0xff0, 2, 0, 400}});
    nr_spr_result_t r;
    ASSERT_EQ(1, nr_spr_estimate(f, a, b, 0.01 / 6, &r)) << "seed " << seed << " zthr " << r.zthr << " nseed " << r.nseed;
    EXPECT_EQ(nr_re_pattern_sym(&r.pat[0]), 0x3300u);
    EXPECT_EQ(nr_re_pattern_sc(&r.pat[0]), 0xff0u);
    EXPECT_EQ(r.pat[0].freq_density, 2);
    EXPECT_EQ(r.pat[0].start_rb, 0);
    EXPECT_GE(r.pat[0].start_rb + r.pat[0].nr_of_rbs, 217); // covers the band the grants saw
    EXPECT_EQ(r.sym_full, 0);
    EXPECT_EQ(r.mod4_left, 0);
    delete f; delete a; delete b;
  }
}

TEST(SlotPhaseResid, TwoResourcesGiveTwoPatternsAndParityIsKept)
{
  std::mt19937 rng(3);
  auto *f = new nr_spr_tab_t(), *a = new nr_spr_tab_t(), *b = new nr_spr_tab_t();
  // A: symbols 8, 9, subcarriers 4..11, every RB.  B: symbol 5 only, sc 0..3, even CRBs only (density 0.5 on the wrong parity would fail)
  FillTabs(rng, f, a, b, 96, 64, {{0x300, 0xff0, 2, 0, 400}, {0x20, 0x00f, 0, 0, 400}});
  nr_spr_result_t r;
  ASSERT_EQ(2, nr_spr_estimate(f, a, b, 0.01 / 6, &r));
  int seen = 0;
  for (int q = 0; q < r.npat; q++) {
    const uint32_t sym = nr_re_pattern_sym(&r.pat[q]), sc = nr_re_pattern_sc(&r.pat[q]);
    if (sym == 0x300) { EXPECT_EQ(sc, 0xff0u); EXPECT_EQ(r.pat[q].freq_density, 2); seen |= 1; }
    if (sym == 0x20) { EXPECT_EQ(sc, 0x00fu); EXPECT_EQ(r.pat[q].freq_density, 0); seen |= 2; }
  }
  EXPECT_EQ(seen, 3);
  delete f; delete a; delete b;
}

TEST(SlotPhaseResid, OddParityResource)
{
  std::mt19937 rng(9);
  auto *f = new nr_spr_tab_t(), *a = new nr_spr_tab_t(), *b = new nr_spr_tab_t();
  FillTabs(rng, f, a, b, 96, 64, {{0x100, 0x111, 1, 0, 400}});
  nr_spr_result_t r;
  ASSERT_EQ(1, nr_spr_estimate(f, a, b, 0.01 / 6, &r));
  EXPECT_EQ(nr_re_pattern_sym(&r.pat[0]), 0x100u);
  EXPECT_EQ(nr_re_pattern_sc(&r.pat[0]), 0x111u);
  EXPECT_EQ(r.pat[0].freq_density, 1);
  delete f; delete a; delete b;
}

TEST(SlotPhaseResid, RbRangeIsTheHullOfTheBucketsInExcess)
{
  std::mt19937 rng(5);
  auto *f = new nr_spr_tab_t(), *a = new nr_spr_tab_t(), *b = new nr_spr_tab_t();
  FillTabs(rng, f, a, b, 160, 64, {{0x3300, 0xff0, 2, 40, 64}}); // CRB 40..103 of a 217-RB band
  nr_spr_result_t r;
  ASSERT_GE(nr_spr_estimate(f, a, b, 0.01 / 6, &r), 1);
  EXPECT_EQ(nr_re_pattern_sym(&r.pat[0]), 0x3300u);
  EXPECT_EQ(r.pat[0].start_rb, 40);
  EXPECT_EQ(r.pat[0].nr_of_rbs, 64);
  delete f; delete a; delete b;
}

TEST(SlotPhaseResid, NoFalseFootprintOnCleanGrants)
{
  int found = 0;
  for (int seed = 1; seed <= 120; seed++) {
    std::mt19937 rng(1000 + seed);
    auto *f = new nr_spr_tab_t(), *a = new nr_spr_tab_t(), *b = new nr_spr_tab_t();
    FillTabs(rng, f, a, b, 96, 64, {});
    nr_spr_result_t r;
    found += nr_spr_estimate(f, a, b, 0.01 / 6, &r) > 0;
    delete f; delete a; delete b;
  }
  EXPECT_EQ(found, 0);
}

TEST(SlotPhaseResid, WholeSymbolTailLossIsReportedNotTurnedIntoAPattern)
{
  std::mt19937 rng(11);
  auto *f = new nr_spr_tab_t(), *a = new nr_spr_tab_t(), *b = new nr_spr_tab_t();
  FillTabs(rng, f, a, b, 96, 64, {{0x2000, 0xfff, 2, 0, 400}}); // symbol 13 entirely
  nr_spr_result_t r;
  EXPECT_EQ(0, nr_spr_estimate(f, a, b, 0.01 / 6, &r));
  EXPECT_TRUE(r.sym_full & 0x2000) << "sym_full 0x" << std::hex << r.sym_full;
  delete f; delete a; delete b;
}

TEST(SlotPhaseResid, TooFewGrantsOrABadGrantGiveNothing)
{
  auto *f = new nr_spr_tab_t(), *a = new nr_spr_tab_t(), *b = new nr_spr_tab_t();
  nr_spr_result_t r;
  EXPECT_EQ(0, nr_spr_estimate(f, a, b, 0.01, &r));
  GTab g;
  memset(&g, 0, sizeof(g)); // no REs at all
  EXPECT_FALSE(nr_spr_add(f, g.e, g.n));
  EXPECT_EQ(f->grants, 0u);
  delete f; delete a; delete b;
}

namespace {
// the cell of the 923 failure: strong grants one per slot; slots % 40 in {8, 18} fail unless the plan applies the measured patterns of the true resource
struct ResidSim {
  std::mt19937 rng{21};
  uint32_t token = 0, gen = 1;
  std::vector<Blk> truth{{0x3300, 0xff0, 2, 0, 400}};
  bool resource_exists = true; // false: the failing phase has no foreign REs (the estimator must stay quiet and the other variants take over)
  uint32_t seeds[2] = {11, 12};
  int tried[12] = {0};
  int wanted_ctrl = 0, wanted_other = 0, wanted_ref = 0;
  bool Matches(uint32_t slot)
  {
    fapi_nr_dl_config_csirs_pdu_rel15_t p[NR_SPR_MAXPAT];
    const int n = nr_slotphase_est_patterns(slot, p, NR_SPR_MAXPAT);
    const bool m = n == 1 && nr_re_pattern_sym(&p[0]) == 0x3300u && nr_re_pattern_sc(&p[0]) == 0xff0u && p[0].freq_density == 2;
    if (!m)
      for (int q = 0; q < n; q++)
        fprintf(stderr, "est pattern %d/%d sym=0x%x sc=0x%x dens=%u rb=%u:%u\n", q, n, nr_re_pattern_sym(&p[q]), nr_re_pattern_sc(&p[q]), p[q].freq_density,
                p[q].start_rb, p[q].nr_of_rbs);
    return m;
  }
  nr_slotphase_choice_t Grant(uint32_t s)
  {
    nr_slotphase_in_t in = {};
    in.seed_ids = seeds;
    in.n_seeds = 2;
    const nr_slotphase_choice_t ch = nr_slotphase_plan(s, gen, ++token, &in);
    const bool bad = (s % 40) == 8 || (s % 40) == 18;
    const bool est = ch.kind == NR_SP_EST || ch.kind == NR_SP_EST_CONF;
    bool ok;
    if (!bad) ok = (rng() % 100) < 95;
    else ok = est && resource_exists && Matches(s) && (rng() % 100) < 90;
    const bool want = nr_slotphase_resid_want(token);
    GTab g;
    if (want) {
      (ch.ent == 0 ? wanted_ref : ch.kind == NR_SP_CTRL ? wanted_ctrl : wanted_other)++;
      // blocked REs are only seen by a grant decoded without rate matching around them
      std::vector<Blk> blk;
      if (bad && resource_exists && !est) blk = truth;
      MakeGrant(g, rng, (int)(rng() % 100), 20 + (int)(rng() % 117), 1 + (int)(rng() % 2), 11 + (int)(rng() % 2), blk);
    }
    nr_slotphase_note_ex(s, gen, token, ok, want ? g.e : nullptr, want ? g.n : nullptr);
    tried[ch.kind]++;
    return ch;
  }
  void Run(uint32_t from, uint32_t slots)
  {
    for (uint32_t s = from; s < from + slots; s++) Grant(s);
  }
};
} // namespace

TEST(SlotPhaseResid, NothingIsAccumulatedOnAHealthyCell)
{
  nr_slotphase_reset();
  ResidSim sim;
  sim.rng.seed(4);
  for (uint32_t s = 0; s < 3000; s++) {
    nr_slotphase_in_t in = {};
    const nr_slotphase_choice_t ch = nr_slotphase_plan(s, 1, ++sim.token, &in);
    EXPECT_FALSE(nr_slotphase_resid_want(sim.token)); // cost ~ 0 where no phase is flagged
    nr_slotphase_note_ex(s, 1, sim.token, (sim.rng() % 100) < 95, nullptr, nullptr);
    EXPECT_EQ(ch.kind, NR_SP_CTRL);
  }
  EXPECT_FALSE(nr_slotphase_active());
}

TEST(SlotPhaseResid, FlaggedPhaseLearnsTheFootprintAdoptsItAndAccumulatesOnlyWhereNeeded)
{
  nr_slotphase_reset();
  ResidSim sim;
  sim.Run(0, 60000);
  int st = -1, ad = -1;
  ASSERT_EQ(nr_slotphase_query(8, &st, &ad), 40);
  uint32_t fg = 0, rg = 0;
  EXPECT_EQ(nr_slotphase_est_state(8, &fg, &rg), NR_SP_EST_POS);
  EXPECT_TRUE(sim.Matches(8));
  EXPECT_TRUE(sim.Matches(18));
  EXPECT_GE(fg, 16u);
  EXPECT_LE(fg, 96u);
  EXPECT_GE(rg, (uint32_t)NR_SLOTPHASE_REF_MIN);
  EXPECT_LE(rg, (uint32_t)NR_SLOTPHASE_REF_TARGET);
  EXPECT_EQ(st, NR_SP_ADOPTED);
  EXPECT_TRUE(ad == 8 || ad == 9); // SV_EST / SV_EST_CONF
  // the accumulation was asked only for control grants of a collecting phase and a bounded reference, never for variant grants
  EXPECT_EQ(sim.wanted_other, 0);
  EXPECT_LE(sim.wanted_ref, NR_SLOTPHASE_REF_TARGET + 64);
  EXPECT_GT(sim.wanted_ctrl, 16);
  EXPECT_LT(sim.wanted_ctrl, 400);
  // adopted: every later grant of the phase uses the measured patterns, the other variants were never tried before the estimate existed
  const nr_slotphase_choice_t ch = [&] { nr_slotphase_in_t in = {}; return nr_slotphase_plan(8 + 40 * 5000, 1, ++sim.token, &in); }();
  EXPECT_TRUE(ch.kind == NR_SP_EST || ch.kind == NR_SP_EST_CONF);
  EXPECT_EQ(sim.tried[NR_SP_UNION] + sim.tried[NR_SP_SEED] + sim.tried[NR_SP_TRIM], 0);
  // once collecting is over nothing more is wanted at the phase
  nr_slotphase_in_t in = {};
  nr_slotphase_plan(8, 1, ++sim.token, &in);
  EXPECT_FALSE(nr_slotphase_resid_want(sim.token));
}

TEST(SlotPhaseResid, NoForeignResourceIsReportedAndTheOtherVariantsFollow)
{
  nr_slotphase_reset();
  ResidSim sim;
  sim.resource_exists = false; // the phase fails for some other reason: clean residual everywhere
  sim.Run(0, 60000);
  uint32_t fg = 0, rg = 0;
  EXPECT_EQ(nr_slotphase_est_state(8, &fg, &rg), NR_SP_EST_NEG);
  EXPECT_EQ(fg, 96u);
  EXPECT_GT(sim.tried[NR_SP_UNION], 0); // the seed variants are tried after the estimator closes
}

TEST(SlotPhaseResid, GrantsWithoutResidualTablesKeepTheOldRotation)
{
  // CPU-demod hosts never deliver tables: the estimator must not hold the phase in control-only mode
  nr_slotphase_reset();
  std::mt19937 rng(2);
  uint32_t tok = 0;
  int tried_union = 0;
  uint32_t seeds[2] = {11, 12};
  for (uint32_t s = 0; s < 30000; s++) {
    nr_slotphase_in_t in = {};
    in.seed_ids = seeds;
    in.n_seeds = 2;
    const nr_slotphase_choice_t ch = nr_slotphase_plan(s, 1, ++tok, &in);
    const bool bad = (s % 40) == 8;
    tried_union += ch.kind == NR_SP_UNION;
    nr_slotphase_note_ex(s, 1, tok, !bad && (rng() % 100) < 95, nullptr, nullptr);
  }
  EXPECT_GT(tried_union, 0);
}

int main(int argc, char **argv)
{
  logInit();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
