#include "nr_sense_quality.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>
#include <cstring>

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
/* A noiseless simulation: the producer's estimate is 0, NaN or absurdly small; every PRB must be valid at the 30 dB ceiling, not invalid. */
TEST(SenseQuality, NoiselessInputIsValidAtTheSnrCeiling)
{
  Cfr c = make(128, [](int) { return 100.f; }, 1.f); /* noise 100 dB below the channel: effectively noiseless */
  for (float nv : {0.f, NAN, 1e-30f, 1e-12f}) {
    auto q = run(c, nv);
    EXPECT_EQ(q.valid_cfr_prbs, 128) << nv;
    EXPECT_NEAR(q.snr_db_p50, 30.f, 0.1f) << nv; /* NR_SENSE_RECON_IMPL_LOSS = 1e-3 */
    EXPECT_TRUE(std::isfinite(q.snr_db_p10));
  }
}
/* No producer estimate on a really noisy record: the local-fit residual gives the SNR; a given estimate is used as is. */
TEST(SenseQuality, MissingNoiseEstimateComesFromTheRecord)
{
  Cfr c = make(128, [](int) { return 15.f; }, 1.f);
  auto given = run(c, 1.f), none = run(c, 0.f), nan = run(c, NAN);
  EXPECT_EQ(given.valid_cfr_prbs, 128);
  EXPECT_NEAR(given.snr_db_p50, 15.f, 1.f);
  EXPECT_EQ(none.valid_cfr_prbs, 128);
  EXPECT_NEAR(none.snr_db_p50, given.snr_db_p50, 1.5f);
  EXPECT_EQ(nan.valid_cfr_prbs, none.valid_cfr_prbs);
  EXPECT_FLOAT_EQ(nan.snr_db_p50, none.snr_db_p50);
}
/* The record-derived estimate must not turn noise into signal: pure noise stays invalid without any producer estimate. */
TEST(SenseQuality, NoiseOnlyWithoutEstimateStaysInvalid)
{
  Cfr c = make(128, [](int) { return -300.f; }, 1.f);
  EXPECT_EQ(run(c, 0.f).valid_cfr_prbs, 0);
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
  /* noise_var 0 = the producer has no estimate: the record's own residual stands in (it used to invalidate every PRB, S3 defect 2026-11-05) */
  auto z = run(c, 0.f);
  EXPECT_EQ(z.valid_cfr_prbs, 128);
  EXPECT_NEAR(z.snr_db_p50, 20.f, 1.5f);
}
TEST(SenseQuality, SnrGateAt6And1Db)
{
  auto q6 = run(make(128, [](int) { return 6.f; }, 1.f), 1.f);
  /* 80-seed Monte-Carlo of this generator (flat per-RE SNR, 128 PRB, 6 RE x 4 ant): 6 dB valid mean 127.74 sd 0.44
   * (min 127), p50 mean 5.99 sd 0.06 (range 5.84..6.15); 1 dB valid mean 0.42 sd 0.63 (max 2). Bounds below are
   * >= 3 sigma from those (6 dB valid >= 125 is 6 sigma; p50 +-0.5 is 8 sigma; 1 dB <= 4 is 5.7 sigma). */
  EXPECT_GE(q6.valid_cfr_prbs, 125);
  EXPECT_NEAR(q6.snr_db_p50, 6.f, 0.5f);
  /* The fixed 3 dB bar is gone: a PRB of 12 REs x 4 antennas is valid when its power sum is above the noise-only 1e-3 tail (about -3 dB SNR), so 1 dB now passes and the
   * rejection point moved down (SenseQualityAdaptive.SnrBarFallsWithNp measures it). Well below the bar nothing passes. */
  auto q1 = run(make(128, [](int) { return 1.f; }, 1.f), 1.f);
  EXPECT_GE(q1.valid_cfr_prbs, 120);
  auto qm = run(make(128, [](int) { return -10.f; }, 1.f), 1.f);
  EXPECT_LE(qm.valid_cfr_prbs, 4);
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
 *   exp(-j 2 pi tau df k)  (delay) and exp(j rho sym)  (per-symbol phase ramp: CFO/Doppler); noise set so the MRC-COMBINED Es/N0 over the 4 antennas = snr_db (the production convention: per-antenna SNR = snr_db - 6 dB). */
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
  const double n0 = Es * d.n_ant / std::pow(10.0, snr_db / 10.0);   /* per-antenna complex noise variance: combined SNR = n_ant * Es / n0 */
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
  /* measured agreement of a perfect-model grant tracks the formula at the combined SNR, minus the local-estimate penalty */
  Da d = make_da(2, 100, 0.f, {}, 0, 0, 0, 3);
  const float c4 = run_da(d);
  printf("S3_RECON qpsk 0 dB combined (4 RX) agreement %.3f (formula 0.708)\n", c4);
  EXPECT_GT(c4, 0.53f); EXPECT_LT(c4, 0.63f);   /* below the 0.708 closed form: 4 RX at -6 dB each make the local estimate poor (modelled, see header) */
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

/* false rejects of good PRBs (perfect model) at the production combined-SNR convention, delay 300 ns + CFO ramp 0.1 rad/symbol */
static int false_rejects(int qm, float snr, int nseed, unsigned base, int *prbs)
{
  int fails = 0; *prbs = 0;
  for (int s = 1; s <= nseed; s++) {
    Da d = make_da(qm, 10, snr, {}, 300e-9, 30e3, 0.1, base + s);
    run_da(d);
    fails += d.nprb - n_ok(d); *prbs += d.nprb;
  }
  return fails;
}
TEST(SenseReconConf, FourRxCombinedSnrFalseRejectRateIsAlphaConsistent)
{
  struct C { int qm; float snr; unsigned base; } cases[] = {{2, 0.f, 5000}, {4, 8.f, 6000}, {6, 12.f, 7000}};
  for (auto c : cases) {
    int prbs, fails = false_rejects(c.qm, c.snr, 100, c.base, &prbs);
    printf("S3_RECON 4RX combined-SNR qm=%d %.0f dB false rejects: %d/%d PRBs (%.2f %%)\n", c.qm, c.snr, fails, prbs, 100.0 * fails / prbs);
    EXPECT_LE(fails, 2) << "qm " << c.qm << ": " << fails << "/" << prbs;   /* limit 2/1000 */
  }
}
TEST(SenseReconConf, ModelledAgreementMatchesMeasuredForQam)
{
  /* M1: the estimate-error cross term uses the constellation Es, so the modelled mean agreement tracks the measured one for 16/64QAM too */
  struct C { int qm; float snr; } cases[] = {{4, 12.f}, {4, 14.f}, {6, 12.f}, {6, 16.f}};   /* lower 16QAM SNRs: the neglected gain bias of the equaliser, header, grows (0.017 at 8 dB) */
  for (auto c : cases) {
    double meas = 0, mod = 0; int cnt = 0;
    for (unsigned seed = 1; seed <= 60; seed++) {
      Da d = make_da(c.qm, 10, c.snr, {}, 300e-9, 30e3, 0.1, 8000 + seed);
      float m = NAN;
      const float a = nr_da_recon_x(d.h.data(), d.n, d.n_ant, d.k.data(), d.l.data(), d.n, d.x_rec.data(), d.qm, c.snr, d.ok, &d.scored, &m);
      meas += a; mod += m; cnt++;
    }
    meas /= cnt; mod /= cnt;
    printf("S3_RECON agreement qm=%d %.0f dB: measured %.4f modelled %.4f\n", c.qm, c.snr, meas, mod);
    EXPECT_NEAR(meas, mod, 0.01) << "qm " << c.qm << " snr " << c.snr;
  }
}
/* keep PRB `p` of d only where pred(symbol index 0..2, subcarrier in PRB) holds */
template <class F> static void thin_prb(Da &d, int p, F pred)
{
  std::vector<int> keep;
  for (int i = 0; i < d.n; i++) if (int(d.k[i] / 12) != p || pred(int(d.l[i]) - 5, int(d.k[i] % 12))) keep.push_back(i);
  Da e = d; e.n = keep.size();
  e.k.clear(); e.l.clear(); e.x_rec.clear(); e.h.assign(2 * d.n_ant * e.n, 0.f);
  for (int j = 0; j < e.n; j++) {
    const int i = keep[j];
    e.k.push_back(d.k[i]); e.l.push_back(d.l[i]); e.x_rec.push_back(d.x_rec[2 * i]); e.x_rec.push_back(d.x_rec[2 * i + 1]);
    for (int a = 0; a < d.n_ant; a++) { e.h[2 * (a * e.n + j)] = d.h[2 * (a * d.n + i)]; e.h[2 * (a * e.n + j) + 1] = d.h[2 * (a * d.n + i) + 1]; }
  }
  d = e;
}
TEST(SenseReconConf, PrbWithTwelveScoredResCanBeRejected)
{
  /* M2: deff = 1 + f W (header) is below 2 at moderate SNR, so a PRB with only 12 scored REs and a grossly wrong reconstruction (independent X)
   * can fail; the old constant halving (deff 2) needed 2x the evidence. PRB 9 keeps the whole middle symbol and subcarriers 0 and 11 of the
   * first/last symbol (the time pair of the two edge REs): exactly 12 scored. QPSK, combined SNR 6 dB / 9 dB; 300 PRBs each. */
  auto thin = [](Da &d) { thin_prb(d, 9, [](int sy, int kk) { return sy == 1 || kk == 0 || kk == 11; }); };
  const int N = 300;
  int rej6 = 0, rej9 = 0, bad_good = 0;
  for (int s = 1; s <= N; s++) {
    Da w6 = make_da(2, 10, 6.f, {9}, 0, 0, 0, 9100 + s); thin(w6); run_da(w6); rej6 += !okbit(w6, 9);
    if (s == 1) EXPECT_EQ(w6.scored - 9 * 32, 12u);
    Da w9 = make_da(2, 10, 9.f, {9}, 0, 0, 0, 9500 + s); thin(w9); run_da(w9); rej9 += !okbit(w9, 9);
    Da g = make_da(2, 10, 6.f, {}, 0, 0, 0, 9900 + s); thin(g); run_da(g); bad_good += !okbit(g, 9);
  }
  printf("S3_RECON 12-RE PRB, wrong model rejected: 6 dB %d/%d, 9 dB %d/%d; good PRB falsely rejected (6 dB) %d/%d\n", rej6, N, rej9, N, bad_good, N);
  EXPECT_GE(rej6, 165);   /* 55 %: derived deff gives about 61 %, the constant deff 2 about 42 % */
  EXPECT_GE(rej9, 285);   /* 95 % */
  EXPECT_LE(bad_good, 2); /* alpha 1e-3 */
}
static int d_mid_offset(int nprb) { return 12 * nprb; }   /* RE index where the middle symbol starts */
TEST(SenseReconConf, OneMismatchAtHighSnrDoesNotFailThePrb)
{
  auto flip = [](Da &d, int i) { for (int a = 0; a < d.n_ant; a++) { d.h[2 * (a * d.n + i)] *= -1; d.h[2 * (a * d.n + i) + 1] *= -1; } };   /* eq = -X: one real mismatch */
  const int prb3_mid = d_mid_offset(10) + 3 * 12;   /* first RE of PRB 3 in the middle symbol */
  Da d = make_da(2, 10, 40.f, {}, 0, 0, 0, 902);
  flip(d, prb3_mid + 5);
  run_da(d);
  EXPECT_EQ(n_ok(d), 10) << "one mismatch against the implementation-loss floor";
  /* a burst of mismatches in one PRB is significant even against the floor */
  Da e = make_da(2, 10, 40.f, {}, 0, 0, 0, 903);
  for (int j = 2; j < 9; j++) flip(e, prb3_mid + j);
  run_da(e);
  EXPECT_FALSE(okbit(e, 3));
  for (int p = 0; p < 10; p++) if (p != 3) EXPECT_TRUE(okbit(e, p)) << p;
}

/* CSI-RS-like input: ONE RE per PRB (subcarrier 1 of each PRB, one symbol), 4 antennas, flat channel of power 4 and noise 0.25 */
static Cfr make_sparse(int nprb, float amp, bool two_symbols, float phase_sym2 = 0.f)
{
  Cfr c;
  for (int s = 0; s < (two_symbols ? 2 : 1); s++)
    for (int p = 0; p < nprb; p++) {
      c.k.push_back(12 * p + 1);
      c.l.push_back(5 + s);
    }
  c.n_re = c.k.size();
  c.iq.resize(2 * c.n_ant * c.n_re);
  for (int a = 0; a < c.n_ant; a++)
    for (int i = 0; i < c.n_re; i++) {
      const float ph = 0.3f * a + (c.l[i] == 6 ? phase_sym2 : 0.f);
      c.iq[2 * (a * c.n_re + i)] = amp * std::cos(ph);
      c.iq[2 * (a * c.n_re + i) + 1] = amp * std::sin(ph);
    }
  return c;
}
TEST(SenseQuality, SparseSourceOneRePerPrbIsValidWithAnEstimate)
{
  Cfr c = make_sparse(106, 2.f, false);
  nr_sense_quality_t q, qd;
  nr_sense_quality_compute_ex(c.n_ant, c.iq.data(), c.n_re, c.n_re, c.k.data(), c.l.data(), 0.25f, NR_SENSE_Q_SPARSE, &q);
  EXPECT_EQ(q.valid_cfr_prbs, 106);
  EXPECT_TRUE(std::isnan(q.resid_db)); /* no local fit */
  EXPECT_GT(q.snr_db_p50, 3.f);
  /* the dense rule still invalidates the same input (tot < 3): the sparse flag is what makes it valid */
  nr_sense_quality_compute(c.n_ant, c.iq.data(), c.n_re, c.n_re, c.k.data(), c.l.data(), 0.25f, &qd);
  EXPECT_EQ(qd.valid_cfr_prbs, 0);
}
TEST(SenseQuality, SparseSourceNeedsANoiseEstimateAndSnr)
{
  Cfr c = make_sparse(2, 2.f, false); /* two PRBs: no triple of equally spaced PRBs, so the frame gives no noise estimate */
  nr_sense_quality_t q;
  nr_sense_quality_compute_ex(c.n_ant, c.iq.data(), c.n_re, c.n_re, c.k.data(), c.l.data(), 0.f, NR_SENSE_Q_SPARSE, &q);
  EXPECT_EQ(q.valid_cfr_prbs, 0); /* nothing to judge the SNR with: not valid */
  nr_sense_quality_compute_ex(c.n_ant, c.iq.data(), c.n_re, c.n_re, c.k.data(), c.l.data(), 4.f, NR_SENSE_Q_SPARSE, &q);
  EXPECT_EQ(q.valid_cfr_prbs, 0); /* the producer's figure stands: |H|^2 = noise, SNR 0 dB < 3 dB */
}
/* CSI-RS-like frame at a known SNR: one RE per PRB, a channel with a delay ramp, white complex noise; the noise is derived from the frame (no producer figure) */
static double sparse_snr_case(double snr_db, int nprb, unsigned* valid, nr_sense_quality_t* q)
{
  Cfr c = make_sparse(nprb, 1.f, false);
  std::mt19937 rng(7);
  std::normal_distribution<double> g(0.0, 1.0);
  const double sigma = std::sqrt(std::pow(10.0, -snr_db / 10.0) / 2.0); /* unit signal power */
  for (int a = 0; a < c.n_ant; a++)
    for (int i = 0; i < c.n_re; i++) {
      const double ph = 0.1 * i + 0.3 * a;
      c.iq[2 * (a * c.n_re + i)] = float(std::cos(ph) + sigma * g(rng));
      c.iq[2 * (a * c.n_re + i) + 1] = float(std::sin(ph) + sigma * g(rng));
    }
  nr_sense_quality_compute_ex(c.n_ant, c.iq.data(), c.n_re, c.n_re, c.k.data(), c.l.data(), 0.f, NR_SENSE_Q_SPARSE, q);
  *valid = q->valid_cfr_prbs;
  return q->snr_db_p50;
}
TEST(SenseQuality, SparseSourceNoiseDerivedFromTheFrameRecoversSnr)
{
  nr_sense_quality_t q;
  unsigned v;
  double s = sparse_snr_case(10.0, 217, &v, &q);
  EXPECT_NEAR(s, 10.0, 2.0);
  EXPECT_GT(v, 200u);
  s = sparse_snr_case(20.0, 217, &v, &q);
  EXPECT_NEAR(s, 20.0, 2.0);
  EXPECT_GT(v, 215u);
  sparse_snr_case(0.0, 217, &v, &q); /* below the 3 dB gate: (almost) no PRB is valid; the p50 is over the PRBs that pass it */
  EXPECT_LT(v, 217u / 4);
}
TEST(SenseQuality, SparseSourceSymbolConsistencyDecidesWithTwoSymbols)
{
  nr_sense_quality_t q;
  Cfr ok = make_sparse(40, 2.f, true, 0.1f);
  nr_sense_quality_compute_ex(ok.n_ant, ok.iq.data(), ok.n_re, ok.n_re, ok.k.data(), ok.l.data(), 0.25f, NR_SENSE_Q_SPARSE, &q);
  EXPECT_EQ(q.valid_cfr_prbs, 40);
  EXPECT_GT(q.sym_consist, 0.9f);
  /* the second symbol decorrelated by pi (anti-phase, the correlation magnitude stays 1): use an unrelated per-PRB phase instead */
  Cfr bad = make_sparse(40, 2.f, true);
  for (int i = 0; i < bad.n_re; i++)
    if (bad.l[i] == 6)
      for (int a = 0; a < bad.n_ant; a++) {
        const float ph = 1.7f * float((i * 37) % 11) + 0.9f * a;
        bad.iq[2 * (a * bad.n_re + i)] = 2.f * std::cos(ph);
        bad.iq[2 * (a * bad.n_re + i) + 1] = 2.f * std::sin(ph);
      }
  nr_sense_quality_compute_ex(bad.n_ant, bad.iq.data(), bad.n_re, bad.n_re, bad.k.data(), bad.l.data(), 0.25f, NR_SENSE_Q_SPARSE, &q);
  EXPECT_LT(q.sym_consist, NR_SENSE_SPARSE_SYM_MIN);
  EXPECT_EQ(q.valid_cfr_prbs, 0);
}

/* ---- per-rule PRB removal counts of a wide record (diagnostic; no rule changes) ---- */
TEST(SenseQuality, InvalCountsPerRuleAndAddUpToTheGrant)
{
  const int nb = 140;
  Cfr c = make(nb, [](int p) { return p < 10 ? -10.f : 20.f; }, 1.f); /* PRBs 0-9: SNR rule */
  std::vector<int> keep;
  std::mt19937_64 g(7);
  std::vector<int> seen(nb, 0);
  for (int i = 0; i < c.n_re; i++) {
    const int p = c.k[i] / 12;
    if (p >= 20 && p < 25) continue;                            /* absent: no RE in the frame */
    if (p >= 10 && p < 15 && ++seen[p] > 2) continue;           /* few_re: 2 REs left */
    keep.push_back(i);
  }
  Cfr d = c;
  d.n_re = keep.size();
  d.k.clear(); d.l.clear(); d.iq.assign(2 * d.n_ant * d.n_re, 0.f);
  for (size_t j = 0; j < keep.size(); j++) {
    d.k.push_back(c.k[keep[j]]); d.l.push_back(c.l[keep[j]]);
    for (int a = 0; a < c.n_ant; a++) {
      float re = c.iq[2 * (a * c.n_re + keep[j])], im = c.iq[2 * (a * c.n_re + keep[j]) + 1];
      const int p = c.k[keep[j]] / 12;
      if (p >= 30 && p < 35) { const float th = float(2 * M_PI * (g() >> 11) / 9007199254740992.0); re = 10.f * std::cos(th), im = 10.f * std::sin(th); } /* resid: 20 dB but no linear structure */
      if (p >= 40 && p < 45) re = im = 0.f;                     /* nosig: no signal power */
      d.iq[2 * (a * d.n_re + j)] = re; d.iq[2 * (a * d.n_re + j) + 1] = im;
    }
  }
  nr_sense_quality_t q = run(d, 1.f);
  uint8_t ok[(NR_SENSE_MAX_PRB + 7) / 8];
  memset(ok, 0xff, sizeof ok);
  for (int p = 50; p < 55; p++) ok[p >> 3] &= (uint8_t)~(1u << (p & 7)); /* recon: PRBs 50-54 fail the reconstruction */
  nr_sense_quality_apply_recon(&q, ok);
  const int removed = nr_sense_quality_absent(&q, nb, false, false);
  EXPECT_EQ(q.inval[NR_SENSE_INV_ABSENT], 5);
  EXPECT_EQ(q.inval[NR_SENSE_INV_TRUNC], 0);
  EXPECT_EQ(q.inval[NR_SENSE_INV_PARTIAL], 0);
  EXPECT_EQ(q.inval[NR_SENSE_INV_FEW_RE], 5);
  EXPECT_EQ(q.inval[NR_SENSE_INV_SNR], 10);
  EXPECT_EQ(q.inval[NR_SENSE_INV_RESID], 5);
  EXPECT_EQ(q.inval[NR_SENSE_INV_NOSIG], 5);
  EXPECT_EQ(q.inval[NR_SENSE_INV_RECON], 5);
  EXPECT_EQ(q.inval[NR_SENSE_INV_SYM], 0);
  EXPECT_EQ(removed, 35);
  EXPECT_EQ(q.valid_cfr_prbs + removed, nb); /* every PRB of the grant is valid or charged to exactly one rule */
  /* the cause of the missing PRBs: truncation first, else a partial TB, else unexplained */
  nr_sense_quality_t t = q;
  memset(t.inval, 0, sizeof t.inval);
  nr_sense_quality_absent(&t, nb, true, true);
  EXPECT_EQ(t.inval[NR_SENSE_INV_TRUNC], 5);
  memset(t.inval, 0, sizeof t.inval);
  nr_sense_quality_absent(&t, nb, false, true);
  EXPECT_EQ(t.inval[NR_SENSE_INV_PARTIAL], 5);
}

TEST(SenseQuality, CleanWideRecordHasNothingRemoved)
{
  Cfr c = make(140, [](int) { return 20.f; }, 1.f);
  nr_sense_quality_t q = run(c, 1.f);
  EXPECT_EQ(nr_sense_quality_absent(&q, 140, false, false), 0);
  EXPECT_EQ(q.valid_cfr_prbs, 140);
}


/* ---- adaptive per-PRB tests (one false-reject level NR_SENSE_PRB_ALPHA = 1e-3, noise from the record) ---- */
namespace {
struct Rng {
  std::mt19937_64 g;
  explicit Rng(unsigned s) : g(s) {}
  float n() { double u1 = ((g() >> 11) + 1) * (1.0 / 9007199254740993.0), u2 = (g() >> 11) * (1.0 / 9007199254740992.0); return float(std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * M_PI * u2)); }
  cf c() { return cf(n(), n()) * float(std::sqrt(0.5)); } /* unit-variance complex */
};
/* the estimator's noise path, simulated independently of nr_sense_quality.c: pilot-pair LS noise (variance 2 per pair mean => s2_p = 2 here, pair mean variance 1) per 4-subcarrier block, then the
 * chest's 16-tap filters (nr_dl_channel_estimation.c TYPE1_linear_interp, UL filters /16384) written at the advancing offset; returns the per-subcarrier noise of nprb PRBs, unit gain. */
std::vector<cf> interp_noise(int nprb, Rng &r, const std::vector<cf> &sig)
{
  const int ns = nprb * 12, cnt = ns / 2;
  std::vector<cf> ls(nprb * 3);
  for (int b = 0; b < nprb * 3; b++) ls[b] = r.c() + sig[b * 4]; /* LS value of block b: signal at its first subcarrier + noise */
  std::vector<cf> y(ns + 16, cf(0, 0));
  const float p0[16] = {.25f, .25f, .25f, .25f, .25f, .25f, .25f, .25f}, p12[16] = {.25f, .25f, .25f, .25f, .125f, .125f, .125f, .125f, .125f, .125f, .125f, .125f},
              mid[16] = {.125f, .125f, .125f, .125f, .125f, .125f, .125f, .125f, .125f, .125f, .125f, .125f, .125f, .125f, .125f, .125f}, last[16] = {.25f, .25f, .25f, .25f, .5f, .5f, .5f, .5f};
  int ptr = 0;
  for (int c = 0; c < cnt; c++) {
    const float *f = c == 0 ? p0 : (c == 1 || c == 2) ? p12 : c == cnt - 1 ? last : mid;
    for (int t = 0; t < 16; t++) y[ptr + t] += f[t] * ls[c / 2];
    if (c > 2 && c != cnt - 1 && c % 2 == 0) ptr += 4;
  }
  y.resize(ns);
  return y;
}
/* white-noise records: n_sym symbols x 12 REs (full grid) or 6-RE comb; flat channel of power snr over noise 1 */
Cfr white_rec(int nprb, int nsym, bool comb, float snr_db, int n_ant, Rng &r, int start_sym = 2)
{
  Cfr c; c.n_ant = n_ant;
  for (int s = 0; s < nsym; s++) for (int p = 0; p < nprb; p++) for (int i = 0; i < (comb ? 6 : 12); i++) { c.k.push_back(12 * p + (comb ? 2 * i : i)); c.l.push_back(start_sym + s); }
  c.n_re = c.k.size(); c.iq.resize(2 * n_ant * c.n_re);
  const float a = std::sqrt(std::pow(10.f, snr_db / 10.f));
  for (int ant = 0; ant < n_ant; ant++) for (int i = 0; i < c.n_re; i++) { cf h = cf(a, 0) * std::polar(1.f, 0.9f * ant) + r.c(); c.iq[2 * (ant * c.n_re + i)] = h.real(); c.iq[2 * (ant * c.n_re + i) + 1] = h.imag(); }
  return c;
}
/* DM-RS decoder-estimate record: comb of the interpolated chest, one symbol, flat channel (unit-gain filters keep it flat) */
Cfr dmrs_rec(int nprb, float snr_db, int n_ant, Rng &r, float a_scale = 1.f)
{
  Cfr c; c.n_ant = n_ant;
  for (int p = 0; p < nprb; p++) for (int i = 0; i < 6; i++) { c.k.push_back(12 * p + 2 * i); c.l.push_back(3); }
  c.n_re = c.k.size(); c.iq.resize(2 * n_ant * c.n_re);
  const float a = a_scale * std::sqrt(std::pow(10.f, snr_db / 10.f) * 0.109375f); /* H noise var of the type-1 chest is 0.109375 s2_p; s2_p = 2 here -> 0.21875: scale the signal to snr over THAT */
  (void)a;
  const float amp = std::sqrt(std::pow(10.f, snr_db / 10.f) * 0.21875f) * a_scale;
  for (int ant = 0; ant < n_ant; ant++) {
    std::vector<cf> sig(nprb * 12, std::polar(amp, 0.9f * ant));
    std::vector<cf> y = interp_noise(nprb, r, sig);
    for (int i = 0; i < c.n_re; i++) { c.iq[2 * (ant * c.n_re + i)] = y[c.k[i]].real(); c.iq[2 * (ant * c.n_re + i) + 1] = y[c.k[i]].imag(); }
  }
  return c;
}
int count_valid_inner(const nr_sense_quality_t &q, int lo, int hi) { int n = 0; for (int p = lo; p < hi; p++) n += bit(q, p); return n; }
}

TEST(SenseQualityAdaptive, DmrsOperatorMatchesIndependentDerivation)
{
  double m[5];
  nr_sense_quality_dmrs_model(m); /* kT, thT, kR, thR, E[S]/s2 per antenna; python derivation of the same filter chain (interior PRB, type 1) */
  EXPECT_NEAR(m[0], 1.3653, 1e-3);
  EXPECT_NEAR(m[0] * m[1], 6.0, 1e-9); /* E[T] = 6 s2 */
  EXPECT_NEAR(m[2], 1.9672, 1e-3);
  EXPECT_NEAR(m[4], 0.16874, 1e-4);
  EXPECT_NEAR(m[2] * m[3], m[4], 1e-9);
  printf("DMRS_MODEL kT=%.4f thT=%.4f kR=%.4f thR=%.5f mR=%.5f\n", m[0], m[1], m[2], m[3], m[4]);
}
/* (i) noise only: PRBs declared valid at ~alpha. White with the exact Gamma: inside binomial bounds. */
TEST(SenseQualityAdaptive, WhiteNoiseOnlyValidAtAlpha)
{
  Rng r(11);
  long valid = 0, tot = 0;
  for (int rec = 0; rec < 1100; rec++) {
    Cfr c = white_rec(100, 2, true, -300.f, 4, r);
    valid += run(c, 1.f).valid_cfr_prbs; tot += 100;
  }
  const double a = 1e-3, mean = a * tot, sd = std::sqrt(tot * a * (1 - a));
  printf("ADAPT white noise-only valid %ld / %ld (alpha*N = %.0f)\n", valid, tot, mean);
  EXPECT_GT(tot, 100000);
  EXPECT_LT(std::fabs(valid - mean), 4 * sd);
}
/* (i) with the noise taken from the record (median reference) the rate stays within a factor 2 of alpha */
TEST(SenseQualityAdaptive, WhiteNoiseOnlyMedianReference)
{
  Rng r(12);
  long valid = 0, tot = 0;
  for (int rec = 0; rec < 1100; rec++) { Cfr c = white_rec(100, 2, true, -300.f, 4, r); valid += run(c, 0.f).valid_cfr_prbs; tot += 100; }
  printf("ADAPT white noise-only valid (median ref) %ld / %ld\n", valid, tot);
  EXPECT_LT(valid, 2.0 * 1e-3 * tot);
}
TEST(SenseQualityAdaptive, DmrsInterpolatedNoiseOnlyValidAtAlpha)
{
  Rng r(13);
  long valid = 0, tot = 0;
  for (int rec = 0; rec < 1300; rec++) {
    Cfr c = dmrs_rec(100, -300.f, 4, r);
    nr_sense_quality_t q; nr_sense_quality_compute_ex(c.n_ant, c.iq.data(), c.n_re, c.n_re, c.k.data(), c.l.data(), 5.f /* ignored */, NR_SENSE_Q_DMRS_INTERP, &q);
    valid += count_valid_inner(q, 3, 97); tot += 94;
  }
  printf("ADAPT dmrs-interp noise-only valid %ld / %ld (alpha*N = %.0f)\n", valid, tot, 1e-3 * tot);
  EXPECT_GT(tot, 100000);
  EXPECT_LT(valid, 3.0 * 1e-3 * tot); /* Satterthwaite tail approximation: bounded within a factor 3 of alpha */
  EXPECT_GT(valid, 0.2 * 1e-3 * tot);
}
/* (ii) signal + noise, model fits: the residual test rejects at ~alpha */
TEST(SenseQualityAdaptive, ResidualRejectsAtAlphaWhenModelFits)
{
  Rng r(14);
  long rej = 0, tot = 0;
  for (int rec = 0; rec < 1100; rec++) { Cfr c = white_rec(100, 2, true, 15.f, 4, r); auto q = run(c, 1.f); rej += q.inval[NR_SENSE_INV_RESID]; tot += 100; }
  printf("ADAPT white resid rejects %ld / %ld\n", rej, tot);
  EXPECT_LT(std::fabs(rej - 1e-3 * tot), 4 * std::sqrt(1e-3 * tot));
  rej = 0; tot = 0;
  for (int rec = 0; rec < 1300; rec++) {
    Cfr c = dmrs_rec(100, 15.f, 4, r);
    nr_sense_quality_t q; nr_sense_quality_compute_ex(c.n_ant, c.iq.data(), c.n_re, c.n_re, c.k.data(), c.l.data(), NAN, NR_SENSE_Q_DMRS_INTERP, &q);
    rej += q.inval[NR_SENSE_INV_RESID]; tot += 100;
  }
  printf("ADAPT dmrs resid rejects %ld / %ld\n", rej, tot);
  EXPECT_LT(rej, 3.0 * 1e-3 * tot);
}
/* 2. another user's REs in one PRB at SNR 10 dB are rejected by the residual test */
TEST(SenseQualityAdaptive, PlantedInterfererRejectedByResidual)
{
  Rng r(15);
  int hit = 0, others_bad = 0;
  for (int rec = 0; rec < 50; rec++) {
    Cfr c = white_rec(60, 2, true, 10.f, 4, r);
    for (int i = 0; i < c.n_re; i++) if (c.k[i] / 12 == 25 && (c.k[i] % 12) == 6) for (int a = 0; a < 4; a++) { c.iq[2 * (a * c.n_re + i)] += 12.f; c.iq[2 * (a * c.n_re + i) + 1] -= 7.f; }
    for (float nv : {1.f, 0.f}) {
      auto q = run(c, nv);
      hit += !bit(q, 25) && q.inval[NR_SENSE_INV_RESID] >= 1;
      others_bad += 59 - (q.valid_cfr_prbs - bit(q, 25));
    }
  }
  EXPECT_EQ(hit, 100);
  EXPECT_LE(others_bad, 15); /* 5900 other PRBs at alpha 1e-3 = 5.9 expected, plus the median reference's own spread (nv = 0 arm) */
}
/* 3. the SNR bar falls as N_p grows: first SNR (0.25 dB steps) where half the PRBs pass */
static double bar50(bool dmrs, Rng &r)
{
  for (double s = -12; s < 12; s += 0.25) {
    int ok = 0, n = 0;
    for (int rec = 0; rec < 20; rec++) {
      Cfr c = dmrs ? dmrs_rec(50, (float)s, 4, r) : white_rec(50, 13, false, (float)s, 4, r);
      nr_sense_quality_t q;
      if (dmrs) nr_sense_quality_compute_ex(c.n_ant, c.iq.data(), c.n_re, c.n_re, c.k.data(), c.l.data(), NAN, NR_SENSE_Q_DMRS_INTERP, &q);
      else nr_sense_quality_compute(c.n_ant, c.iq.data(), c.n_re, c.n_re, c.k.data(), c.l.data(), 1.f, &q);
      ok += count_valid_inner(q, 3, 47); n += 44;
    }
    if (ok * 2 >= n) return s;
  }
  return 99;
}
TEST(SenseQualityAdaptive, SnrBarFallsWithNp)
{
  Rng r(16);
  const double dm = bar50(true, r), da = bar50(false, r);
  printf("ADAPT SNR bar (50%% pass) dm-rs PRB (6 RE) %.2f dB, data PRB (156 RE) %.2f dB\n", dm, da);
  EXPECT_LT(da, dm - 3.0);
}
/* 4. one PRB with 20 dB extra noise leaves the other PRBs' verdicts unchanged and is itself rejected */
TEST(SenseQualityAdaptive, MedianReferenceIgnoresOneBadPrb)
{
  Rng r(17);
  for (int rec = 0; rec < 20; rec++) {
    Cfr c = white_rec(60, 2, true, 12.f, 4, r);
    auto base = run(c, 0.f);
    Rng r2(1000 + rec);
    for (int i = 0; i < c.n_re; i++) if (c.k[i] / 12 == 30) for (int a = 0; a < 4; a++) { cf x = 10.f * r2.c(); c.iq[2 * (a * c.n_re + i)] += x.real(); c.iq[2 * (a * c.n_re + i) + 1] += x.imag(); }
    auto bad = run(c, 0.f);
    EXPECT_FALSE(bit(bad, 30));
    for (int p = 0; p < 60; p++) if (p != 30) EXPECT_EQ(bit(bad, p), bit(base, p)) << p;
  }
}
