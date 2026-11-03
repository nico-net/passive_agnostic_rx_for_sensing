/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */
/* test_nr_ul_energy_probe: the ULEN maths (energy over a PRB mask, timing-agnostic DM-RS correlation vs a wrong pilot). */
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>
#include <gtest/gtest.h>
extern "C" {
#include "nr_ul_energy_probe.h"
}

namespace {
constexpr int N = 1024, NRB = 40, FCO = N - 12 * NRB / 2;
const int OFF[6] = {0, 2, 4, 6, 8, 10};

c16_t pilot_at(int idx, int seed)
{
  uint32_t h = (uint32_t)idx * 2654435761u ^ (uint32_t)seed * 2246822519u;
  h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
  return c16_t{(int16_t)((h & 1) ? 8000 : -8000), (int16_t)((h & 2) ? 8000 : -8000)};
}
} // namespace

TEST(UlEnergyProbe, EnergyOverPrbMaskExcludesTheGrant)
{
  std::vector<c16_t> row(N, c16_t{0, 0});
  for (int p = 0; p < NRB; p++)
    for (int r = 0; r < 12; r++) {
      const int a = (p >= 10 && p < 14) ? 100 : 10; /* grant on PRBs 10..13 */
      row[(FCO + p * 12 + r) % N] = c16_t{(int16_t)a, 0};
    }
  double s = 0, u = 0;
  uint64_t cs = 0, cu = 0;
  nr_ulen_energy_acc(row.data(), N, FCO, 10, 14, 14, 14, nullptr, 12, &s, &cs);
  nr_ulen_energy_acc(row.data(), N, FCO, 2, NRB - 2, 10, 14, nullptr, 12, &u, &cu);
  EXPECT_EQ(cs, 48u);
  EXPECT_EQ(cu, (uint64_t)(NRB - 4 - 4) * 12);
  EXPECT_NEAR(nr_ulen_db(s, cs), 40.0, 1e-9);
  EXPECT_NEAR(nr_ulen_db(u, cu), 20.0, 1e-9); /* the grant's PRBs are not in the unscheduled mean */
  EXPECT_LT(nr_ulen_db(0, 0), -90.0);
}

TEST(UlEnergyProbe, CorrelationFindsInjectedDmrsAtItsDelayAndRejectsAWrongPilot)
{
  const int delay = 128; /* samples the signal is LATE in the window: the ramp that removes it is tau = -delay */
  std::vector<c16_t> pil(6 * NRB), wrong(6 * NRB), row(N);
  std::mt19937 rng(7);
  std::normal_distribution<float> g(0.f, 300.f);
  for (int i = 0; i < 6 * NRB; i++) { pil[i] = pilot_at(i, 1); wrong[i] = pilot_at(i, 2); }
  for (int k = 0; k < N; k++) row[k] = c16_t{(int16_t)g(rng), (int16_t)g(rng)};
  for (int p = 10; p < 30; p++)
    for (int j = 0; j < 6; j++) {
      const int k = (FCO + 12 * p + OFF[j]) % N;
      /* transmitted = conj(pilot), times the channel phase of the group and the delay ramp */
      const double ph = -2.0 * M_PI * (double)(12 * p + OFF[j]) * delay / N + 0.7 * (p / 2);
      const double tr = pil[6 * p + j].r, ti = -pil[6 * p + j].i;
      const double a = 1.0;
      row[k] = c16_t{(int16_t)lrint(a * (tr * cos(ph) - ti * sin(ph))), (int16_t)lrint(a * (tr * sin(ph) + ti * cos(ph)))};
    }
  int taus[NR_ULEN_MAX_TAU], nt = 0;
  taus[nt++] = 0;
  for (int i = 1; i <= 16; i++) { taus[nt++] = i * 16; taus[nt++] = -i * 16; }
  const c16_t *rows[1] = {row.data()};
  const c16_t *pr[1] = {pil.data()}, *pw[1] = {wrong.data()};
  int bt = 0, bn = 0;
  const double c = nr_ulen_corr_best(rows, pr, 1, N, FCO, 10, 20, OFF, 6, taus, nt, &bt);
  const double cn = nr_ulen_corr_best(rows, pw, 1, N, FCO, 10, 20, OFF, 6, taus, nt, &bn);
  EXPECT_EQ(bt, -delay);
  EXPECT_GT(c, 0.5);
  EXPECT_LT(cn, 0.3);
  EXPECT_GT(c, 2.0 * cn);
}

TEST(UlEnergyProbe, NoiseOnlyGivesTheSameCorrelationForBothPilots)
{
  std::vector<c16_t> pil(6 * NRB), wrong(6 * NRB), row(N);
  std::mt19937 rng(11);
  std::normal_distribution<float> g(0.f, 300.f);
  for (int i = 0; i < 6 * NRB; i++) { pil[i] = pilot_at(i, 1); wrong[i] = pilot_at(i, 2); }
  for (int k = 0; k < N; k++) row[k] = c16_t{(int16_t)g(rng), (int16_t)g(rng)};
  int taus[3] = {0, 16, -16}, b1, b2;
  const c16_t *rows[1] = {row.data()};
  const c16_t *pr[1] = {pil.data()}, *pw[1] = {wrong.data()};
  const double c = nr_ulen_corr_best(rows, pr, 1, N, FCO, 10, 20, OFF, 6, taus, 3, &b1);
  const double cn = nr_ulen_corr_best(rows, pw, 1, N, FCO, 10, 20, OFF, 6, taus, 3, &b2);
  EXPECT_LT(c, 0.3);
  EXPECT_LT(cn, 0.3);
}
