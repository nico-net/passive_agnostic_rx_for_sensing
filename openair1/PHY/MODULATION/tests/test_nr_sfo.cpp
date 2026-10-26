/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* nr_sfo_state / nr_sfo_rot: accuracy of the integer phasor, CPU table form == per-RE form, physics of the correction, eps provider. */
#include <gtest/gtest.h>
#include <cmath>
#include <complex>
#include <vector>
#include <random>
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
extern "C" {
#include "nr_sfo_state.h"
configmodule_interface_t *uniqCfg = NULL;
void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
  abort();
}
}

TEST(Sfo, SinCosAccuracy)
{
  double worst = 0;
  for (uint64_t ph = 0; ph < (1ull << 32); ph += 9973 * 4099ull) {
    const nr_sfo_c16_t v = nr_sfo_sincos((uint32_t)ph);
    const double a = (double)ph * 2 * M_PI / 4294967296.0;
    worst = std::max(worst, std::abs(v.r - std::min(32767.0, 32768 * cos(a))));
    worst = std::max(worst, std::abs(v.i - std::min(32767.0, 32768 * sin(a))));
  }
  EXPECT_LE(worst, 1.0); /* within one Q15 LSB */
}

TEST(Sfo, PhasorMatchesExp)
{
  const uint32_t dphi = nr_sfo_dphi(4250000, 4096, 50000); /* 4.25 ppm, ~symbol 12 */
  double worst = 0;
  for (int k = -2048; k < 2048; k += 7) {
    const nr_sfo_c16_t p = nr_sfo_phasor(k, dphi);
    const double a = (double)k * (double)dphi * 2 * M_PI / 4294967296.0;
    worst = std::max(worst, std::abs(std::complex<double>(p.r, p.i) / 32768.0 - std::polar(1.0, a)));
  }
  EXPECT_LT(worst, 2e-4);
}

/* the CPU symbol loop (tables per symbol) and the per-RE form a GPU thread computes give the same int16 bit for bit */
TEST(Sfo, TableLoopEqualsPerRe)
{
  std::mt19937 rng(7);
  for (int N : {512, 1024, 2048, 4096}) {
    for (int nb_rb : {51, 106, 133, 273}) {
      if (nb_rb * 12 > N) continue;
      const int fco = N - nb_rb * 6;
      std::vector<int16_t> a(2 * N), b;
      for (auto &x : a) x = (int16_t)(rng() & 0xffff);
      b = a;
      const uint32_t dphi = (uint32_t)rng();
      const int kref = (int)(rng() % (unsigned)N) - N / 2; /* any reference inside the band */
      nr_sfo_rotate_symbol(a.data(), N, fco, nb_rb, dphi, kref);
      const int len = (nb_rb & 1) ? (nb_rb + 1) * 6 : nb_rb * 6, start2 = (nb_rb & 1) ? fco - 6 : fco;
      for (int n = 0; n < N; n++) {
        nr_sfo_c16_t v = {b[2 * n], b[2 * n + 1]};
        if (n < len || (n >= start2 && n < start2 + len)) v = nr_sfo_re(v, nr_sfo_bin_k(n, N) - kref, dphi);
        ASSERT_EQ(a[2 * n], v.r) << "N " << N << " rb " << nb_rb << " bin " << n;
        ASSERT_EQ(a[2 * n + 1], v.i) << "N " << N << " rb " << nb_rb << " bin " << n;
      }
    }
  }
}

/* a symbol received eps * n_l samples late (receiver clock fast) is X[k] exp(-j 2 pi k eps n_l / N); the correction restores X[k] */
TEST(Sfo, UndoesTheDriftOfAFastClock)
{
  const int N = 4096, nb_rb = 273, fco = N - nb_rb * 6;
  const int64_t eps_q = 3800000; /* +3.8 ppm */
  const int n_l = nr_sfo_symbol_offset(N, 288, 352, 1, 14, 3, 11);
  EXPECT_EQ(n_l, 11 * (4096 + 288));
  const double delay = eps_q * 1e-12 * n_l;
  const int kref = nr_sfo_kref(N, fco, 150); /* SSB at subcarrier 150 of the carrier */
  EXPECT_EQ(kref, 150 + 120 - nb_rb * 6);
  std::vector<int16_t> s(2 * N, 0);
  std::vector<std::complex<double>> x(N);
  std::mt19937 rng(3);
  double err = 0, ref = 0, err_unc = 0;
  for (int n = 0; n < N; n++) {
    const int k = nr_sfo_bin_k(n, N);
    if (!(n < nb_rb * 6 + 6 || n >= fco - 6)) continue;
    x[n] = std::complex<double>((int)(rng() % 2) ? 8000 : -8000, (int)(rng() % 2) ? 8000 : -8000);
    /* the frequency offset is measured on the SSB (k_ref): its drift there is already compensated */
    const std::complex<double> y = x[n] * std::polar(1.0, -2 * M_PI * (k - kref) * delay / N);
    s[2 * n] = (int16_t)lround(y.real());
    s[2 * n + 1] = (int16_t)lround(y.imag());
    err_unc += std::norm(y - x[n]);
  }
  nr_sfo_rotate_symbol(s.data(), N, fco, nb_rb, nr_sfo_dphi(eps_q, N, n_l), kref);
  for (int n = 0; n < N; n++) {
    if (x[n] == 0.0) continue;
    err += std::norm(std::complex<double>(s[2 * n], s[2 * n + 1]) - x[n]);
    ref += std::norm(x[n]);
  }
  EXPECT_GT(10 * log10(ref / err), 60.0);        /* corrected: quantisation only */
  EXPECT_LT(10 * log10(ref / err_unc), 25.0);    /* uncorrected: the impairment this fixes */
}

TEST(Sfo, ProviderSignAndHysteresis)
{
  nr_sfo_reset();
  const int spf = 1228800;
  EXPECT_EQ(nr_sfo_eps_q(), 0);
  /* fast receiver clock, +4.25 ppm: the timing loop reads ~5.22 samples more per frame (integer reads: 5 or 6) */
  double acc = 0;
  for (int f = 0; f < 1024; f++) {
    acc += 4.25e-6 * spf;
    const int extra = (int)lround(acc);
    acc -= extra;
    nr_sfo_note_frame_read(extra, spf);
  }
  EXPECT_NEAR(nr_sfo_eps(), 4.25e-6, 0.01e-6);
  const int64_t before = nr_sfo_eps_q();
  for (int f = 0; f < 256; f++) { /* +0.005 ppm: below the hysteresis */
    acc += 4.255e-6 * spf;
    const int extra = (int)lround(acc);
    acc -= extra;
    nr_sfo_note_frame_read(extra, spf);
  }
  EXPECT_EQ(nr_sfo_eps_q(), before);
  /* a timing jump is not drift */
  nr_sfo_note_frame_read(5000, spf);
  EXPECT_EQ(nr_sfo_eps_q(), before);
  nr_sfo_pin(123);
  EXPECT_EQ(nr_sfo_eps_q_for_fep(), 123);
  nr_sfo_pin(INT64_MIN);
  EXPECT_EQ(nr_sfo_eps_q_for_fep(), before);
  /* no drift: no correction at all (bit-identical FEP) */
  nr_sfo_reset();
  for (int f = 0; f < 512; f++) nr_sfo_note_frame_read(0, spf);
  EXPECT_EQ(nr_sfo_eps_q(), 0);
  EXPECT_EQ(nr_sfo_dphi(nr_sfo_eps_q(), 4096, 40000), 0u);
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  uniqCfg = load_configmodule(argc, argv, CONFIG_ENABLECMDLINEONLY);
  logInit();
  return RUN_ALL_TESTS();
}
