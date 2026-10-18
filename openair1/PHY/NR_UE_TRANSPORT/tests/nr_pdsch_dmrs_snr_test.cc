// Estimator of EVMGRANT dmrs_snr / dmrs_ref on synthetic DM-RS: the real chest recipe (pair-averaged LS repeated 4 / 6 times, the 16-tap FIR of
// nr_dl_channel_estimation.c, int16 storage, nres = mean |LS - h|^2) fed with a flat channel of known SNR per RE.
#include <gtest/gtest.h>
#include <chrono>
#include <cmath>
#include <random>
#include <vector>
extern "C" {
#include "nr_pdsch_dmrs_snr.h"
}
namespace {
const double kP0[16] = {4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 0, 0, 0, 0, 0, 0, 0, 0};
const double kP12[16] = {4096, 4096, 4096, 4096, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 0, 0, 0, 0};
const double kMid[16] = {2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048};
const double kLast[16] = {4096, 4096, 4096, 4096, 8192, 8192, 8192, 8192, 0, 0, 0, 0, 0, 0, 0, 0};

struct Est { std::vector<int16_t> h; uint32_t nres; };

// one antenna, nrb PRBs, channel |h|^2 = s_pow (flat), noise sigma^2 per RE = s_pow / snr_lin (0 snr_lin -> noise only with sigma2 given by s_pow)
Est chest(std::mt19937 &g, int type, int nrb, double s_pow, double sigma2)
{
  std::normal_distribution<double> nd(0.0, 1.0);
  const int rep = type == 1 ? 4 : 6, npair = type == 1 ? 3 : 2, idx = 12 * nrb;
  std::vector<int16_t> ls(2 * idx, 0);
  const double a = std::sqrt(s_pow / 2.0), sd = std::sqrt(sigma2 / 2.0);
  for (int q = 0; q < nrb * npair; q++) {
    double r = a + (nd(g) * sd + nd(g) * sd) / 2, i = a + (nd(g) * sd + nd(g) * sd) / 2;
    for (int k = 0; k < rep; k++) { ls[2 * (q * rep + k)] = (int16_t)std::lround(r); ls[2 * (q * rep + k) + 1] = (int16_t)std::lround(i); }
  }
  std::vector<double> h(2 * (idx + 32), 0.0);
  int base = 0;
  for (int pc = 0; pc < idx >> 1; pc++) {
    const double *f = pc == 0 ? kP0 : (pc == 1 || pc == 2) ? kP12 : pc == (idx >> 1) - 1 ? kLast : kMid;
    for (int i = 0; i < 16; i++) { h[2 * (base + i)] += ls[4 * pc] * f[i] / 16384.0; h[2 * (base + i) + 1] += ls[4 * pc + 1] * f[i] / 16384.0; }
    if (!(pc <= 2 || pc == (idx >> 1) - 1) && pc % 2 == 0) base += 4;
  }
  Est e;
  e.h.resize(2 * idx);
  double res = 0;
  for (int k = 0; k < 2 * idx; k++) {
    e.h[k] = (int16_t)std::lround(h[k]);
    res += (double)(ls[k] - e.h[k]) * (ls[k] - e.h[k]);
  }
  e.nres = (uint32_t)(res / idx);
  return e;
}
double snr_db(const Est &e, int type)
{
  double pw;
  return nr_dmrs_db(nr_dmrs_snr_ant(e.h.data(), (int)e.h.size() / 2, e.nres, type, &pw));
}
} // namespace

TEST(DmrsSnr, KnownSnrWithinOneDb)
{
  std::mt19937 g(7);
  for (int type = 1; type <= 2; type++)
    for (int db : {0, 10, 20})
      for (int rep = 0; rep < 5; rep++) {
        const double s = 2000.0 * 2000.0 * 2.0; // |h|^2
        const Est e = chest(g, type, 100, s, s / std::pow(10.0, db / 10.0));
        const double got = snr_db(e, type);
        if (rep == 0) printf("type %d true %2d dB -> est %.2f dB\n", type, db, got);
        EXPECT_NEAR(got, db, 1.0) << "type " << type << " snr " << db;
      }
  // small grant (edges weigh more)
  for (int db : {10, 20}) EXPECT_NEAR(snr_db(chest(g, 1, 8, 8e6, 8e6 / std::pow(10.0, db / 10.0)), 1), db, 1.5);
}

TEST(DmrsSnr, NoiseOnlyIsStronglyNegative)
{
  std::mt19937 g(11);
  for (int type = 1; type <= 2; type++)
    for (int rep = 0; rep < 5; rep++) {
      const double got = snr_db(chest(g, type, 100, 0.0, 1e6), type);
      if (rep == 0) printf("type %d noise only -> %.2f dB\n", type, got);
      EXPECT_LT(got, -10.0) << "type " << type;
    }
}

TEST(DmrsSnr, CombIsMrcSumAndMissingAntennaIsMinus99)
{
  std::mt19937 g(3);
  const Est a = chest(g, 1, 50, 8e6, 8e5), b = chest(g, 1, 50, 8e6, 8e5);
  const int16_t *rows[4] = {a.h.data(), b.h.data(), nullptr, nullptr};
  const uint32_t nres[4] = {a.nres, b.nres, 0, 0};
  nr_dmrs_snr_t s;
  nr_dmrs_snr_init(&s);
  nr_dmrs_snr_fill(&s, rows, 4, 600, nres, 1, 1.0);
  EXPECT_NEAR(s.comb_db, s.snr_db[0] + 3.0, 0.6);
  EXPECT_EQ(s.snr_db[2], -99.f);
  EXPECT_EQ(s.snr_db[3], -99.f);
  EXPECT_NEAR(s.pw_db, 10 * std::log10(8e6 + 8e5), 1.0); // S + sigma^2
}

TEST(DmrsSnr, RefExcludesGrantAndWraps)
{
  const int N = 64;
  std::vector<int16_t> rx(2 * N, 0);
  for (int k = 0; k < N; k++) rx[2 * k] = 10;           // power 100 everywhere
  for (int k = 50; k < 60; k++) rx[2 * k] = 1000;       // grant [50, 60) mod 64 is loud
  EXPECT_NEAR(nr_dmrs_ref_db(rx.data(), N, 40, 40, 50, 10), 20.0, 1e-3); // carrier [40, 80) -> wraps to 16; grant excluded
  EXPECT_EQ(nr_dmrs_ref_db(rx.data(), N, 40, 40, 70, 20), -99.f);        // grant leaves the carrier
  std::vector<int16_t> big(2 * 4096, 7);
  const Est e = chest(*new std::mt19937(1), 1, 106, 8e6, 8e5);
  volatile float sink = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 1000; i++) {
    sink = sink + nr_dmrs_ref_db(big.data(), 4096, 400 + (i & 3), 3300, 1000, 1272);
    for (int a = 0; a < 4; a++) sink = sink + (float)snr_db(e, 1);
  }
  const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 1000;
  printf("per grant (ref + 4 antennas): %.2f us\n", us);
  EXPECT_LT(us, 20.0);
}
