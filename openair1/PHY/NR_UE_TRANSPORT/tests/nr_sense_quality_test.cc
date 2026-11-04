#include "nr_sense_quality.h"
#include <gtest/gtest.h>
#include <algorithm>
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

/* ---- S3: data-aided reconstruction (nr_da_recon) and the PRB-validity join ---- */
#include "nr_pdsch_da_recon.h"

namespace {
struct Da {
  int nprb, n_ant, n, qm;
  float snr_db;
  std::vector<float> h;         /* H = Y/X_rec, antenna-major, stride n */
  std::vector<uint32_t> k, l;
  std::vector<int16_t> x_rec;
  uint8_t ok[(NR_SENSE_MAX_PRB + 7) / 8];
  uint32_t scored = 0;
};
/* qm bits/symbol, 3 data symbols (REs ordered symbol by symbol, k ascending). The transmitter sends x_true; the receiver re-encodes
 * x_rec = x_true except in PRBs listed in wrong_prb, where it is independent (a wrong reconstruction).
 * Channel per antenna and PRB: unit-modulus random gain (a precoder change at every PRB boundary) times
 *   exp(-j 2 pi tau df k)  (delay) and exp(j rho sym)  (per-symbol phase ramp: CFO/Doppler); noise set so Es/N0 = snr_db exactly in expectation. */
Da make_da(int qm, int nprb, float snr_db, std::vector<int> wrong_prb, double tau_s, double df_hz, double rho, unsigned seed)
{
  Da d; d.nprb = nprb; d.n_ant = 4; d.qm = qm; d.snr_db = snr_db;
  std::mt19937_64 g(seed);
  const int M = 1 << (qm / 2);
  auto gauss = [&g]() {
    double u1 = ((g() >> 11) + 1) * (1.0 / 9007199254740993.0), u2 = (g() >> 11) * (1.0 / 9007199254740992.0);
    return float(std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * M_PI * u2));
  };
  auto sym = [&]() { return int16_t(1000 * (2 * int(g() % M) - (M - 1))); };
  for (int s = 0; s < 3; s++) for (int kk = 0; kk < 12 * nprb; kk++) { d.k.push_back(kk); d.l.push_back(5 + s); }
  d.n = d.k.size();
  d.h.resize(2 * d.n_ant * d.n);
  const double Es = 2.0 * 1e6 * (double(M) * M - 1.0) / 3.0;        /* E|X|^2 with unit u = 1000 */
  const double n0 = Es / std::pow(10.0, snr_db / 10.0);             /* complex noise variance */
  std::vector<cf> hp(d.n_ant * nprb);
  for (auto &v : hp) { double a = 2.0 * M_PI * double(g() % 100000) / 100000.0; v = cf(std::cos(a), std::sin(a)); }
  for (int i = 0; i < d.n; i++) {
    const int p = d.k[i] / 12;
    const bool wr = std::find(wrong_prb.begin(), wrong_prb.end(), p) != wrong_prb.end();
    int16_t tr = sym(), ti = sym();
    int16_t rr = wr ? sym() : tr, ri = wr ? sym() : ti;
    d.x_rec.push_back(rr); d.x_rec.push_back(ri);
    const double ph = -2.0 * M_PI * tau_s * df_hz * d.k[i] + rho * (d.l[i] - 5);
    for (int a = 0; a < d.n_ant; a++) {
      cf y = hp[a * nprb + p] * cf(std::cos(ph), std::sin(ph)) * cf(tr, ti) + cf(gauss(), gauss()) * float(std::sqrt(n0 / 2));
      cf hh = y / cf(rr, ri);   /* H = Y/X with the reconstructed X */
      d.h[2 * (a * d.n + i)] = hh.real(); d.h[2 * (a * d.n + i) + 1] = hh.imag();
    }
  }
  return d;
}
float run_da(Da &d, float snr_db) { return nr_da_recon(d.h.data(), d.n, d.n_ant, d.k.data(), d.l.data(), d.n, d.x_rec.data(), d.qm, snr_db, d.ok, &d.scored); }
float run_da(Da &d) { return run_da(d, d.snr_db); }
bool okbit(const Da &d, int p) { return d.ok[p >> 3] >> (p & 7) & 1; }
int n_ok(const Da &d) { int c = 0; for (int p = 0; p < d.nprb; p++) c += okbit(d, p); return c; }
}

TEST(SenseReconConf, ExpectedAgreementMatchesTheClosedForm)
{
  /* QPSK at Es/N0 = 0 dB: P_axis = Q(1) = 0.1587, p = 0.708 ; 16QAM at 10 dB: P_axis = 2*0.75*Q(sqrt(3*10/15)) = 0.0786.. */
  EXPECT_NEAR(0.5 * std::erfc(1.0 / std::sqrt(2.0)), 0.158655, 1e-6);
  /* measured agreement of a perfect-model grant tracks the formula: one antenna = the formula minus the local-estimate penalty (0.708 -> ~0.66);
   * four antennas add 6 dB of array gain (QPSK 6 dB: (1-Q(2))^2 = 0.955 -> ~0.91), which the verdict ignores on purpose (conservative) */
  Da d = make_da(2, 100, 0.f, {}, 0, 0, 0, 3);
  const float c4 = run_da(d);
  const float c1 = nr_da_recon(d.h.data(), d.n, 1, d.k.data(), d.l.data(), d.n, d.x_rec.data(), d.qm, 0.f, d.ok, nullptr);
  printf("S3_RECON qpsk 0 dB agreement: 1 ant %.3f, 4 ant %.3f (formula 0.708 / 0.955)\n", c1, c4);
  EXPECT_GT(c1, 0.60f); EXPECT_LT(c1, 0.708f);
  EXPECT_GT(c4, 0.86f); EXPECT_LT(c4, 0.955f);
}
TEST(SenseReconConf, PerfectModelKeepsAllPrbsAtLowCodeRateSnrs)
{
  /* Es/N0 where a rate-1/3 .. 1/2 code just decodes: raw agreement is far below 1, yet every PRB must stay valid */
  struct C { int qm; float snr; } cases[] = {{2, -1.f}, {2, 0.f}, {2, 3.f}, {4, 5.f}, {4, 8.f}, {6, 12.f}, {8, 20.f}};
  for (auto c : cases) {
    int fails = 0, prbs = 0;
    for (unsigned seed = 1; seed <= 40; seed++) {
      Da d = make_da(c.qm, 10, c.snr, {}, 300e-9, 30e3, 0.1, seed);
      run_da(d);
      fails += d.nprb - n_ok(d); prbs += d.nprb;
    }
    EXPECT_LE(fails, 1) << "qm " << c.qm << " snr " << c.snr << ": false rejects " << fails << "/" << prbs;   /* alpha 1e-3 -> 0.4 expected of 400 */
    printf("S3_RECON false-reject qm=%d snr=%.0f dB: %d/%d PRBs\n", c.qm, c.snr, fails, prbs);
  }
}
TEST(SenseReconConf, LowSnrIsNotRejectedForBeingLowSnr)
{
  for (int qm : {2, 4, 6}) {
    int fails = 0;
    for (unsigned seed = 1; seed <= 20; seed++) {
      Da d = make_da(qm, 10, -3.f, {}, 0, 0, 0, 100 + seed);   /* far below the decoding SNR of any code */
      run_da(d);
      fails += d.nprb - n_ok(d);
    }
    EXPECT_LE(fails, 1) << "qm " << qm;
  }
}
TEST(SenseReconConf, WrongModelFailsWhenExpectationIsAboveChance)
{
  struct C { int qm; float snr; } cases[] = {{2, 6.f}, {2, 10.f}, {4, 14.f}, {6, 20.f}};
  for (auto c : cases) {
    Da all = make_da(c.qm, 10, c.snr, {0,1,2,3,4,5,6,7,8,9}, 0, 0, 0, 21 + c.qm);
    run_da(all);
    EXPECT_EQ(n_ok(all), 0) << "qm " << c.qm << " snr " << c.snr;
    /* one wrong PRB in the middle: its verdict fails, the neighbours (other precoding) stay valid: no window crosses a PRB boundary */
    Da one = make_da(c.qm, 10, c.snr, {4}, 0, 0, 0, 31 + c.qm);
    run_da(one);
    for (int p = 0; p < 10; p++) EXPECT_EQ(okbit(one, p), p != 4) << "qm " << c.qm << " prb " << p;
  }
}
TEST(SenseReconConf, EdgeSubcarriersAreScoredFromTimePairs)
{
  Da d = make_da(4, 10, 14.f, {}, 0, 0, 0, 77);
  run_da(d);
  /* middle symbol: all 12 subcarriers (0 and 11 through the +-1 symbol pairs); first/last symbol: subcarriers 1..10 only (0 and 11 have no
   * frequency pair inside the PRB and only one neighbouring symbol, hence no time pair): 10 x (12 + 10 + 10) scored */
  EXPECT_EQ(d.scored, uint32_t(10 * 32));
}
TEST(SenseReconConf, NoSnrNoVerdict)
{
  Da d = make_da(4, 10, 14.f, {}, 0, 0, 0, 78);
  EXPECT_TRUE(std::isnan(run_da(d, NAN)));
  for (int p = 0; p < 10; p++) EXPECT_FALSE(okbit(d, p));
}
TEST(SenseReconConf, NoBiasFromDelaySlopeOrPhaseRamp)
{
  for (int qm : {4, 6, 8}) {
    const float snr = qm == 4 ? 25.f : (qm == 6 ? 30.f : 36.f);
    Da flat = make_da(qm, 10, snr, {}, 0, 0, 0, 40 + qm);
    Da dly = make_da(qm, 10, snr, {}, 300e-9, 30e3, 0, 40 + qm);
    Da ramp = make_da(qm, 10, snr, {}, 0, 0, 0.1, 40 + qm);
    Da both = make_da(qm, 10, snr, {}, 300e-9, 30e3, 0.1, 40 + qm);
    const float f = run_da(flat), a = run_da(dly), b = run_da(ramp), c = run_da(both);
    EXPECT_GT(f, 0.97f) << qm;
    EXPECT_NEAR(a, f, 0.02f) << "delay slope, qm " << qm;
    EXPECT_NEAR(b, f, 0.02f) << "phase ramp, qm " << qm;
    EXPECT_NEAR(c, f, 0.02f) << "both, qm " << qm;
    EXPECT_EQ(n_ok(both), 10) << qm;
  }
}
TEST(SenseReconConf, TooFewSymbolsOrBadOrderIsNanAndNoVerdict)
{
  Da d = make_da(4, 1, 14.f, {}, 0, 0, 0, 5);   /* 36 REs */
  EXPECT_TRUE(std::isnan(run_da(d)));
  for (int p = 0; p < 275; p++) EXPECT_FALSE(okbit(d, p));
  Da e = make_da(4, 10, 14.f, {}, 0, 0, 0, 6);
  e.qm = 3;
  EXPECT_TRUE(std::isnan(run_da(e)));
  Da f = make_da(8, 2, 30.f, {}, 0, 0, 0, 7);   /* 72 REs < 256 for 256QAM: innermost level may be absent */
  EXPECT_TRUE(std::isnan(run_da(f)));
}
TEST(SenseReconConf, InconsistentConstellationScaleIsNan)
{
  Da d = make_da(6, 10, 20.f, {}, 0, 0, 0, 9);
  for (size_t i = 0; i < d.x_rec.size(); i++) if (d.x_rec[i] == 1000 || d.x_rec[i] == -1000) d.x_rec[i] = int16_t(d.x_rec[i] * 3);   /* no innermost level: u would be 3000 */
  EXPECT_TRUE(std::isnan(run_da(d)));
}
TEST(SenseReconConf, BacklogSkipIsAtHalfThePool)
{
  EXPECT_FALSE(nr_da_recon_skip(0, 32)); EXPECT_FALSE(nr_da_recon_skip(15, 32));
  EXPECT_TRUE(nr_da_recon_skip(16, 32)); EXPECT_TRUE(nr_da_recon_skip(32, 32));
  EXPECT_FALSE(nr_da_recon_skip(0, 0));
}
TEST(SenseReconConf, BinomialTailIsExact)
{
  /* the project helper the verdict uses: P(Bin(10,0.5) >= 10) = 1/1024, and the lower tail identity used for the agreement count */
  EXPECT_NEAR(nbs_binom_sf(10, 10, 0.5), 1.0 / 1024, 1e-12);
  EXPECT_NEAR(nbs_binom_sf(8, 10, 0.1), 45 * 1e-8 * 0.81 + 10 * 1e-9 * 0.9 + 1e-10 /* C(10,8) .1^8 .9^2 + C(10,9) .1^9 .9 + .1^10 */, 1e-12);
}

TEST(SenseQualityRecon, ApplyReconAndsTheMaskAndRecounts)
{
  Cfr c = make(20, [](int) { return 20.f; }, 1.f);
  for (auto &k : c.k) k += 12 * 7;   /* first_prb = 7 */
  auto q = run(c, 1.f);
  ASSERT_EQ(q.valid_cfr_prbs, 20);
  ASSERT_EQ(q.first_prb, 7);
  uint8_t ok[(NR_SENSE_MAX_PRB + 7) / 8] = {0};
  for (int a = 7; a < 17; a++) ok[a >> 3] |= (uint8_t)(1u << (a & 7));   /* absolute PRBs 7..16 pass */
  nr_sense_quality_apply_recon(&q, ok);
  EXPECT_EQ(q.valid_cfr_prbs, 10);
  for (int p = 0; p < 20; p++) EXPECT_EQ(bit(q, p), p < 10) << p;
  memset(ok, 0, sizeof ok);
  nr_sense_quality_apply_recon(&q, ok);
  EXPECT_EQ(q.valid_cfr_prbs, 0);
}
