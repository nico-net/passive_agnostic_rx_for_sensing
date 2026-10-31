#include <cmath>
#include <cstdint>
#include <random>
#include <gtest/gtest.h>
extern "C" {
#include "nr_ul_dmrs_diag.h"
}

namespace {
constexpr int N = 1024, NRB = 40, SPS = 14, FCO = N - 12 * NRB / 2;

struct Scene {
  int sig_rb0 = 10, sig_n = 4, sig_sym = 2, sig_sd = 0, delay = 0; /* delay: samples the signal is LATE in the nominal window */
  float amp = 40.f, sigma = 10.f;
  bool signal = true;
  int off[6] = {0, 2, 4, 6, 8, 10};
};

c16_t hashq(int sd, int sym, bool ctl, int idx)
{
  uint32_t h = (uint32_t)(sd + 3) * 2654435761u ^ (uint32_t)sym * 40503u ^ (ctl ? 0x9e3779b9u : 0u) ^ (uint32_t)idx * 2246822519u;
  h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
  return c16_t{(int16_t)((h & 1) ? 16000 : -16000), (int16_t)((h & 2) ? 16000 : -16000)};
}

bool pilot(void *, int sd, int sym, bool ctl, c16_t *out)
{
  for (int rb = 0; rb < NRB; rb++)
    for (int j = 0; j < 6; j++) out[6 * rb + j] = hashq(sd, sym, ctl, 6 * rb + j);
  return true;
}

bool fep(void *u, int ant, int sd, int sym, int adv, c16_t *dst)
{
  const Scene *sc = (const Scene *)u;
  std::mt19937 rng((unsigned)(ant * 7919 + (sd + 2) * 104729 + sym * 1299709)); /* the same noise for every window start */
  std::normal_distribution<float> g(0.f, sc->sigma);
  for (int k = 0; k < N; k++) dst[k] = c16_t{(int16_t)g(rng), (int16_t)g(rng)};
  if (sc->signal && sd == sc->sig_sd && sym == sc->sig_sym) {
    const double d = sc->delay + adv; /* an earlier window pushes the content later */
    for (int rb = sc->sig_rb0; rb < sc->sig_rb0 + sc->sig_n; rb++)
      for (int j = 0; j < 6; j++) {
        const int krel = 12 * rb + sc->off[j];
        const c16_t p = hashq(sd, sym, false, 6 * rb + j);
        const double ph = -2.0 * M_PI * krel * d / N;
        const double tr = p.r / 16000.0, ti = -p.i / 16000.0; /* tx = conj(pilot) */
        const double yr = (tr * cos(ph) - ti * sin(ph)) * sc->amp, yi = (tr * sin(ph) + ti * cos(ph)) * sc->amp;
        c16_t &y = dst[(FCO + krel) % N];
        y.r = (int16_t)(y.r + yr);
        y.i = (int16_t)(y.i + yi);
      }
  }
  return true;
}

nr_ul_dmrs_diag_in_t make(Scene *sc, int claim0, int claim_n, int dmrs_sym)
{
  nr_ul_dmrs_diag_in_t in{};
  in.fft = N; in.nrb_carrier = NRB; in.first_carrier = FCO; in.nant = 2; in.symbols_per_slot = SPS;
  in.bwp_start = 0; in.bwp_size = NRB; in.claim_start = claim0; in.claim_n = claim_n; in.dmrs_sym = dmrs_sym;
  in.grant_first_sym = 0; in.grant_n_sym = 14; in.dmrs_mask = (uint16_t)(1u << dmrs_sym);
  in.nre = 6;
  for (int j = 0; j < 6; j++) in.re_off[j] = sc->off[j];
  in.fep = fep; in.pilot = pilot; in.u = sc;
  return in;
}
} // namespace

TEST(UlDmrsDiag, ClaimedPlaceAtOffsetZero)
{
  Scene sc;
  auto in = make(&sc, 10, 4, 2);
  nr_ul_dmrs_diag_out_t o;
  ASSERT_TRUE(nr_ul_dmrs_diag_run(&in, &o));
  ASSERT_TRUE(o.valid && o.have_corr);
  EXPECT_GT(o.e_claim_db, o.e_floor_db + 3.f);
  EXPECT_GT(o.e_claim_db, o.e_unclaim_db + 3.f);
  EXPECT_GT(o.snr_claim_db, -5.f);
  EXPECT_EQ(o.best_rb_off, 0);
  EXPECT_EQ(o.best_sym_off, 0);
  EXPECT_EQ(o.best_slot_off, 0);
  EXPECT_LE(std::abs(o.best_t_off), 8);
}

TEST(UlDmrsDiag, NoiseOnlyStaysAtTheFloor)
{
  Scene sc;
  sc.signal = false;
  auto in = make(&sc, 10, 4, 2);
  nr_ul_dmrs_diag_out_t o;
  ASSERT_TRUE(nr_ul_dmrs_diag_run(&in, &o));
  EXPECT_LT(o.e_claim_db, o.e_floor_db + 2.f);
  EXPECT_LT(o.e_claim_db, 2.5f);
  EXPECT_LT(o.e_rb_db, o.fl_rb_db + 2.f);
}

TEST(UlDmrsDiag, EnergyAtAnotherPrbIsFound)
{
  Scene sc;
  sc.sig_rb0 = 22;
  auto in = make(&sc, 10, 4, 2);
  nr_ul_dmrs_diag_out_t o;
  ASSERT_TRUE(nr_ul_dmrs_diag_run(&in, &o));
  EXPECT_EQ(o.best_rb_off, 12);
  EXPECT_GT(o.e_rb_db, o.fl_rb_db + 2.f); /* the statistic saturates at 10log10(nre)=7.8 dB, so the margin over the maximised control is small by construction */
  EXPECT_LT(o.e_claim_db, o.e_floor_db + 2.f);
}

TEST(UlDmrsDiag, LateSignalGivesTheTimingToApply)
{
  /* 200 samples late: starting the window 200 samples LATER (a negative advance) catches it */
  Scene sc;
  sc.delay = 200;
  auto in = make(&sc, 10, 4, 2);
  nr_ul_dmrs_diag_out_t o;
  ASSERT_TRUE(nr_ul_dmrs_diag_run(&in, &o));
  EXPECT_LE(std::abs(o.best_t_off + 200), 8);
  EXPECT_GT(o.e_t_db, o.fl_t_db + 3.f);
  EXPECT_LE(std::abs(o.best_rb_t_off + 200), 8);
  EXPECT_EQ(o.best_rb_off, 0);
}

TEST(UlDmrsDiag, WrongSymbolAndWrongSlot)
{
  Scene s1;
  s1.sig_sym = 9;
  auto in = make(&s1, 10, 4, 2);
  nr_ul_dmrs_diag_out_t o;
  ASSERT_TRUE(nr_ul_dmrs_diag_run(&in, &o));
  EXPECT_EQ(o.best_sym_off, 7);
  EXPECT_GT(o.e_sym_db, o.fl_sym_db + 3.f);
  Scene s2;
  s2.sig_sd = 1;
  auto in2 = make(&s2, 10, 4, 2);
  ASSERT_TRUE(nr_ul_dmrs_diag_run(&in2, &o));
  EXPECT_EQ(o.best_slot_off, 1);
  EXPECT_GT(o.e_slot_db, o.fl_slot_db + 3.f);
}

TEST(UlDmrsDiag, TransformPrecodingGivesPowerOnly)
{
  Scene sc;
  auto in = make(&sc, 10, 4, 2);
  in.pilot = nullptr;
  nr_ul_dmrs_diag_out_t o;
  ASSERT_TRUE(nr_ul_dmrs_diag_run(&in, &o));
  EXPECT_TRUE(o.valid);
  EXPECT_FALSE(o.have_corr);
}

TEST(UlDmrsDiag, RejectsBadGeometry)
{
  Scene sc;
  auto in = make(&sc, 38, 4, 2);
  nr_ul_dmrs_diag_out_t o;
  EXPECT_FALSE(nr_ul_dmrs_diag_run(&in, &o));
}
