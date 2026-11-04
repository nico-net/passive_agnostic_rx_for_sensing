#include "nr_sense_quality.h"
#include <gtest/gtest.h>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>

typedef std::complex<float> cf;
struct Cfr {
  int n_ant = 4, n_re = 0;
  std::vector<float> iq;
  std::vector<uint16_t> k;
  std::vector<uint8_t> l;
};
static bool bit(const nr_sense_quality_t &q, int p) { return q.valid_mask[p >> 3] >> (p & 7) & 1; }

/* 2-tap channel (unit mean power), DM-RS-like even k on symbols 2 and 3; per-PRB signal power scale via snr_db(p). */
template <class F> static Cfr make(int nprb, F snr_db, float noise_var, unsigned seed = 1)
{
  Cfr c;
  std::mt19937_64 g(seed);
  auto n = [&g]() { /* Box-Muller on explicit uniforms: no std::normal_distribution (implementation-defined) */
    double u1 = ((g() >> 11) + 1) * (1.0 / 9007199254740993.0), u2 = (g() >> 11) * (1.0 / 9007199254740992.0);
    return float(std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * M_PI * u2));
  };
  for (int s = 2; s <= 3; s++)
    for (int p = 0; p < nprb; p++)
      for (int i = 0; i < 6; i++) {
        c.k.push_back(12 * p + 2 * i);
        c.l.push_back(s);
      }
  c.n_re = c.k.size();
  c.iq.resize(2 * c.n_ant * c.n_re);
  for (int a = 0; a < c.n_ant; a++)
    for (int i = 0; i < c.n_re; i++) {
      float th = -2.f * float(M_PI) * 2.f * c.k[i] / 4096.f + 0.7f * a;
      cf h0 = cf(1.f, 0.f) + 0.5f * cf(std::cos(th), std::sin(th));
      /* normalised per (antenna, k): |signal|^2 is exactly noise_var * 10^(snr/10) at every RE */
      cf h = std::sqrt(noise_var * std::pow(10.f, snr_db(c.k[i] / 12) / 10.f) / std::norm(h0)) * h0;
      float nr = n(), ni = n();
      h += std::sqrt(noise_var / 2) * cf(nr, ni);
      c.iq[2 * (a * c.n_re + i)] = h.real();
      c.iq[2 * (a * c.n_re + i) + 1] = h.imag();
    }
  return c;
}
static nr_sense_quality_t run(const Cfr &c, float nv)
{
  nr_sense_quality_t q;
  nr_sense_quality_compute(c.n_ant, c.iq.data(), c.n_re, c.n_re, c.k.data(), c.l.data(), nv, &q);
  return q;
}

TEST(SenseQuality, CleanChannelAllValid)
{
  Cfr c = make(128, [](int) { return 20.f; }, 1.f);
  auto q = run(c, 1.f);
  EXPECT_EQ(q.valid_cfr_prbs, 128);
  EXPECT_EQ(q.allocated_prbs, 128);
  EXPECT_EQ(q.estimated_usable_prbs, 128);
  EXPECT_NEAR(q.snr_db_p50, 20.f, 1.f);
  EXPECT_GT(q.sym_consist, 0.9f);
  EXPECT_TRUE(std::isnan(q.recon_conf));
}
TEST(SenseQuality, NoiseOnlyNoneValid)
{
  Cfr c = make(128, [](int) { return -300.f; }, 1.f);
  auto q = run(c, 1.f);
  EXPECT_EQ(q.valid_cfr_prbs, 0);
  EXPECT_TRUE(std::isnan(q.snr_db_p50));
  EXPECT_TRUE(std::isnan(q.resid_db));
}
TEST(SenseQuality, HalfBandFaded)
{
  Cfr c = make(128, [](int p) { return p < 64 ? 20.f : -10.f; }, 1.f);
  auto q = run(c, 1.f);
  EXPECT_EQ(q.valid_cfr_prbs, 64);
  for (int p = 0; p < 128; p++)
    EXPECT_EQ(bit(q, p), p < 64) << p;
}
TEST(SenseQuality, ResidualFlagsCorruptPrb)
{
  Cfr c = make(128, [](int) { return 20.f; }, 1.f);
  std::mt19937 g(7); /* uniform_real_distribution below only draws phases, bounds are not tight */
  std::uniform_real_distribution<float> u(0.f, 6.2831853f);
  for (int i = 0; i < c.n_re; i++)
    if (c.k[i] / 12 >= 20 && c.k[i] / 12 < 30)
      for (int a = 0; a < c.n_ant; a++) {
        cf h(c.iq[2 * (a * c.n_re + i)], c.iq[2 * (a * c.n_re + i) + 1]);
        h = std::polar(std::abs(h), u(g));
        c.iq[2 * (a * c.n_re + i)] = h.real();
        c.iq[2 * (a * c.n_re + i) + 1] = h.imag();
      }
  auto q = run(c, 1.f);
  EXPECT_EQ(q.valid_cfr_prbs, 118);
  for (int p = 0; p < 128; p++)
    EXPECT_EQ(bit(q, p), p < 20 || p >= 30) << p;
}
TEST(SenseQuality, PhaseJumpAcrossPrbKeepsAllValid)
{ /* a PRG precoder change: per-PRB phase step; per-PRB fit must be unaffected */
  Cfr c = make(128, [](int) { return 20.f; }, 1.f);
  for (int i = 0; i < c.n_re; i++) {
    cf r = std::polar(1.f, 1.3f * float(c.k[i] / 12 % 5));
    for (int a = 0; a < c.n_ant; a++) {
      cf h(c.iq[2 * (a * c.n_re + i)], c.iq[2 * (a * c.n_re + i) + 1]);
      h *= r;
      c.iq[2 * (a * c.n_re + i)] = h.real();
      c.iq[2 * (a * c.n_re + i) + 1] = h.imag();
    }
  }
  EXPECT_EQ(run(c, 1.f).valid_cfr_prbs, 128);
}
TEST(SenseQuality, DegeneratePrbInvalid)
{
  Cfr c = make(128, [](int) { return 20.f; }, 1.f);
  Cfr d;
  d.n_ant = c.n_ant;
  d.n_re = 0;
  std::vector<int> keep;
  int seen = 0; /* PRB 5 keeps only 2 REs of symbol 2 */
  for (int i = 0; i < c.n_re; i++)
    if (c.k[i] / 12 != 5 || (c.l[i] == 2 && seen++ < 2))
      keep.push_back(i);
  d.n_re = keep.size();
  d.iq.resize(2 * d.n_ant * d.n_re);
  for (int j = 0; j < d.n_re; j++) {
    d.k.push_back(c.k[keep[j]]);
    d.l.push_back(c.l[keep[j]]);
    for (int a = 0; a < d.n_ant; a++) {
      d.iq[2 * (a * d.n_re + j)] = c.iq[2 * (a * c.n_re + keep[j])];
      d.iq[2 * (a * d.n_re + j) + 1] = c.iq[2 * (a * c.n_re + keep[j]) + 1];
    }
  }
  auto q = run(d, 1.f);
  EXPECT_FALSE(bit(q, 5));
  EXPECT_EQ(q.valid_cfr_prbs, 127);
  auto z = run(c, 0.f);
  EXPECT_EQ(z.valid_cfr_prbs, 0);
  for (unsigned char b : z.valid_mask)
    EXPECT_EQ(b, 0);
  EXPECT_TRUE(std::isnan(z.snr_db_p50));
}
TEST(SenseQuality, SnrGateAt6And1Db)
{
  auto q6 = run(make(128, [](int) { return 6.f; }, 1.f), 1.f);
  /* 80-seed Monte-Carlo of this generator (flat per-RE SNR, 128 PRB, 6 RE x 4 ant): 6 dB valid mean 127.74 sd 0.44
   * (min 127), p50 mean 5.99 sd 0.06 (range 5.84..6.15); 1 dB valid mean 0.42 sd 0.63 (max 2). Bounds below are
   * >= 3 sigma from those (6 dB valid >= 125 is 6 sigma; p50 +-0.5 is 8 sigma; 1 dB <= 4 is 5.7 sigma). */
  EXPECT_GE(q6.valid_cfr_prbs, 125);
  EXPECT_NEAR(q6.snr_db_p50, 6.f, 0.5f);
  auto q1 = run(make(128, [](int) { return 1.f; }, 1.f), 1.f);
  EXPECT_LE(q1.valid_cfr_prbs, 4); /* the old (S+N)/N SNR reads 3.5 dB here and passes ~40 */
}
TEST(SenseQuality, NonZeroFirstPrbMaskOffset)
{
  Cfr c = make(64, [](int p) { return p < 10 ? -10.f : 20.f; }, 1.f);
  for (auto &k : c.k)
    k += 12 * 40;
  auto q = run(c, 1.f);
  EXPECT_EQ(q.first_prb, 40);
  EXPECT_EQ(q.valid_cfr_prbs, 54);
  for (int p = 0; p < 64; p++)
    EXPECT_EQ(bit(q, p), p >= 10) << p;
  EXPECT_FALSE(bit(q, 64));
}
TEST(SenseQuality, InfInFitSymbolInvalid)
{
  Cfr c = make(16, [](int) { return 20.f; }, 1.f);
  for (int i = 0; i < c.n_re; i++)
    if (c.k[i] / 12 == 3)
      c.iq[2 * i] = INFINITY;
  auto q = run(c, 1.f);
  EXPECT_FALSE(bit(q, 3));
  EXPECT_EQ(q.valid_cfr_prbs, 15);
}
TEST(SenseQuality, SingleSymbolConsistNan)
{
  Cfr c = make(16, [](int) { return 20.f; }, 1.f);
  c.n_re /= 2; /* keep symbol 2 only; stride stays the full allocation */
  c.k.resize(c.n_re);
  c.l.resize(c.n_re);
  nr_sense_quality_t q;
  nr_sense_quality_compute(c.n_ant, c.iq.data(), 2 * c.n_re, c.n_re, c.k.data(), c.l.data(), 1.f, &q);
  EXPECT_TRUE(std::isnan(q.sym_consist));
}
TEST(SenseQuality, BenchNsPerCall)
{
  Cfr c = make(128, [](int) { return 20.f; }, 1.f);
  nr_sense_quality_t q;
  const int N = 2000;
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < N; i++) {
    nr_sense_quality_compute(c.n_ant, c.iq.data(), c.n_re, c.n_re, c.k.data(), c.l.data(), 1.f, &q);
    asm volatile("" ::"r"(&q) : "memory");
  }
  double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / N;
  printf("BENCH nr_sense_quality_compute 128PRBx4antx2sym: %.0f ns/call\n", ns);
}

/* ---- S3: data-aided reconstruction confidence (nr_da_recon_conf) ---- */
#include "nr_pdsch_da_recon.h"

namespace {
struct Da {
  int nprb, n_ant, n;
  std::vector<float> h;         /* H = Y/X, antenna-major, stride n */
  std::vector<uint32_t> k;
  std::vector<int16_t> x_true, x_rec;
};
/* qm bits/symbol; the transmitter sends x_true; the receiver reconstructs x_rec (equal, or independent = wrong decode) */
Da make_da(int qm, int nprb, float noise, bool wrong, unsigned seed)
{
  Da d; d.nprb = nprb; d.n_ant = 4;
  std::mt19937_64 g(seed);
  const int M = 1 << (qm / 2);
  auto gauss = [&g]() {
    double u1 = ((g() >> 11) + 1) * (1.0 / 9007199254740993.0), u2 = (g() >> 11) * (1.0 / 9007199254740992.0);
    return float(std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * M_PI * u2));
  };
  auto sym = [&]() { return int16_t(1000 * (2 * int(g() % M) - (M - 1))); };
  for (int p = 0; p < nprb; p++) for (int i = 0; i < 12; i++) for (int s = 0; s < 3; s++) d.k.push_back(12 * p + i);
  d.n = d.k.size();
  d.h.resize(2 * d.n_ant * d.n);
  std::vector<cf> hp(d.n_ant * nprb);
  for (auto &v : hp) v = cf(gauss(), gauss());
  for (int i = 0; i < d.n; i++) {
    int16_t tr = sym(), ti = sym();
    d.x_true.push_back(tr); d.x_true.push_back(ti);
    int16_t rr = wrong ? sym() : tr, ri = wrong ? sym() : ti;
    d.x_rec.push_back(rr); d.x_rec.push_back(ri);
    for (int a = 0; a < d.n_ant; a++) {
      cf y = hp[a * nprb + d.k[i] / 12] * cf(tr, ti) + cf(gauss(), gauss()) * noise * 1000.f;
      cf hh = y / cf(rr, ri);   /* H = Y/X with the reconstructed X */
      d.h[2 * (a * d.n + i)] = hh.real(); d.h[2 * (a * d.n + i) + 1] = hh.imag();
    }
  }
  return d;
}
}

TEST(SenseReconConf, CorrectReconstructionScoresHighAllOrders)
{
  for (int qm : {2, 4, 6, 8}) {
    Da d = make_da(qm, 10, 0.02f, false, 11 + qm);
    float c = nr_da_recon_conf(d.h.data(), d.n, d.n_ant, d.k.data(), d.n, d.x_rec.data(), qm);
    EXPECT_GT(c, 0.97f) << "qm " << qm;
    EXPECT_LE(c, 1.0f);
  }
}
TEST(SenseReconConf, WrongReconstructionScoresLow)
{
  for (int qm : {2, 4, 6}) {
    Da d = make_da(qm, 10, 0.02f, true, 21 + qm);
    float c = nr_da_recon_conf(d.h.data(), d.n, d.n_ant, d.k.data(), d.n, d.x_rec.data(), qm);
    EXPECT_LT(c, 0.5f) << "qm " << qm;
  }
}
TEST(SenseReconConf, TooFewSymbolsOrBadOrderIsNan)
{
  Da d = make_da(4, 1, 0.02f, false, 5);   /* 36 REs */
  EXPECT_TRUE(std::isnan(nr_da_recon_conf(d.h.data(), d.n, d.n_ant, d.k.data(), d.n, d.x_rec.data(), 4)));
  Da e = make_da(4, 10, 0.02f, false, 6);
  EXPECT_TRUE(std::isnan(nr_da_recon_conf(e.h.data(), e.n, e.n_ant, e.k.data(), e.n, e.x_rec.data(), 3)));
}
