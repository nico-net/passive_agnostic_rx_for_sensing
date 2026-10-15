/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 *
 * ISAC_PUSCH_MMSE_MODE=2: unbiased 2-layer UL equaliser (nr_mmse_2layers_unbiased) against the stock
 * nr_mmse_2layers(), on synthetic 2-layer 256QAM through OAI's own nr_channel_compensation().
 *   kappa = mean(|Re z|, |Im z|) / mean(threshold n1): an unbiased equaliser gives ~1 (mean |coord| of 256QAM per
 *   dimension = 8/sqrt(170) = QAM256_n1). Stock with diagonal loading on correlated layers: < 1 (QAM-FIX, DL twin).
 * Flag off is the stock function itself (the UL call site only branches on ISAC_PUSCH_MMSE_MODE == 2), checked
 * byte for byte against a second stock call on the same input.
 */
#include <gtest/gtest.h>
#include "../nr_ulsch_passive_branch.h"
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

struct c16_t {
  int16_t r, i;
};
extern "C" {
void nr_channel_compensation(uint32_t buffer_length, uint32_t pdsch_buf_size_max, int nb_rx_ant, int nb_layers,
                             void *rxFext, void *chFext, void *ch_maga, void *ch_magb, void *ch_magc, c16_t **rxComp,
                             void *rho, int mod_order, uint32_t symbol, uint32_t output_shift);
uint8_t nr_mmse_2layers(c16_t **rxdataF_comp, uint32_t buffer_length, uint32_t pdsch_buf_size_max, int nb_rx_ant,
                        int nb_layers, void *ch_mag, void *ch_magb, void *ch_magc, void *ch_estimates_ext,
                        unsigned short nb_rb, unsigned char mod_order, int shift, unsigned char symbol, int length,
                        uint32_t noise_var);
void nr_mmse_2layers_unbiased(c16_t **rxdataF_comp, uint32_t buffer_length, uint32_t pdsch_buf_size_max,
                              int nb_rx_ant, int nb_layers, void *ch_mag, void *ch_magb, void *ch_magc,
                              void *ch_estimates_ext, unsigned char mod_order, int shift, unsigned char symbol,
                              int length);
}
struct configmodule_interface_s;
extern "C" struct configmodule_interface_s *uniqCfg = nullptr;
extern "C" void exit_function(const char *, const char *, const int, const char *, const int) { abort(); }

namespace {
constexpr int kRx = 4, kL = 2, kRb = 50, kRe = kRb * 12, kBuf = 608; // ceil_mod(600, 16)
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed) {}
  uint32_t next() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return (uint32_t)(s >> 33); }
  double uni() { return (next() + 0.5) / 4294967296.0; }
  double gauss() { return std::sqrt(-2.0 * std::log(uni())) * std::cos(2.0 * M_PI * uni()); }
};

struct Case {
  alignas(64) c16_t rxf[kRx][kBuf];
  alignas(64) c16_t chf[kL][kRx][kBuf];
  alignas(64) c16_t maga[kL][kBuf], magb[kL][kBuf], magc[kL][kBuf];
  alignas(64) c16_t comp[kL][kBuf];
  alignas(64) c16_t rho[kL][kL][kBuf];
  int sym[kL][kRe][2]; // transmitted 256QAM coordinates (odd integers -15..15)
};

/* y = H s (+ noise), s = 256QAM coordinates / sqrt(170); layer 1 = corr * layer 0 + independent part. */
void Build(Case &c, double corr, double h_rms, double snr_db, uint64_t seed, int shift)
{
  memset(&c, 0, sizeof(c));
  Rng g(seed);
  const double nstd = h_rms / std::sqrt(2.0) * std::pow(10.0, -snr_db / 20.0);
  for (int k = 0; k < kRe; k++) {
    double hr[kL][kRx], hi[kL][kRx];
    for (int r = 0; r < kRx; r++) {
      const double ar = g.gauss() * h_rms / std::sqrt(2.0), ai = g.gauss() * h_rms / std::sqrt(2.0);
      const double br = g.gauss() * h_rms / std::sqrt(2.0), bi = g.gauss() * h_rms / std::sqrt(2.0);
      hr[0][r] = ar; hi[0][r] = ai;
      hr[1][r] = corr * ar + std::sqrt(1 - corr * corr) * br;
      hi[1][r] = corr * ai + std::sqrt(1 - corr * corr) * bi;
    }
    double sr[kL], si[kL];
    for (int l = 0; l < kL; l++) {
      c.sym[l][k][0] = 2 * (int)(g.next() % 16) - 15;
      c.sym[l][k][1] = 2 * (int)(g.next() % 16) - 15;
      sr[l] = c.sym[l][k][0] / std::sqrt(170.0);
      si[l] = c.sym[l][k][1] / std::sqrt(170.0);
    }
    for (int r = 0; r < kRx; r++) {
      double yr = 0, yi = 0;
      for (int l = 0; l < kL; l++) {
        c.chf[l][r][k] = {(int16_t)std::lround(hr[l][r]), (int16_t)std::lround(hi[l][r])};
        yr += c.chf[l][r][k].r * sr[l] - c.chf[l][r][k].i * si[l];
        yi += c.chf[l][r][k].r * si[l] + c.chf[l][r][k].i * sr[l];
      }
      if (snr_db < 90) { yr += g.gauss() * nstd; yi += g.gauss() * nstd; }
      c.rxf[r][k] = {(int16_t)std::lround(yr), (int16_t)std::lround(yi)};
    }
  }
  c16_t *comp[kL] = {c.comp[0], c.comp[1]};
  nr_channel_compensation(kBuf, kBuf, kRx, kL, c.rxf, c.chf, c.maga, c.magb, c.magc, comp, c.rho, 8, 0, shift);
}

double Kappa(const Case &c)
{
  double num = 0, den = 0;
  for (int l = 0; l < kL; l++)
    for (int k = 0; k < kRe; k++) {
      num += (std::abs(c.comp[l][k].r) + std::abs(c.comp[l][k].i)) / 2.0;
      den += c.maga[l][k].r;
    }
  return den > 0 ? num / den : 0;
}

}  // namespace

constexpr int kShift = 10;

TEST(PuschMmse2, StockIsBiasedOnCorrelatedLayersModeTwoIsNot)
{
  static Case a, b;
  Build(a, 0.8, 600, 99, 11, kShift);
  memcpy(&b, &a, sizeof(a));
  c16_t *ca[kL] = {a.comp[0], a.comp[1]}, *cb[kL] = {b.comp[0], b.comp[1]};
  const uint32_t nvar = (uint32_t)(0.1 * kRx * 600.0 * 600.0); // diagonal load ~10 % of G_ll, the gNB's noise scale
  nr_mmse_2layers(ca, kBuf, kBuf, kRx, kL, a.maga, a.magb, a.magc, a.chf, kRb, 8, kShift, 0, kRe, nvar);
  nr_mmse_2layers_unbiased(cb, kBuf, kBuf, kRx, kL, b.maga, b.magb, b.magc, b.chf, 8, kShift, 0, kRe);
  const double ks = Kappa(a), k2 = Kappa(b);
  printf("kappa stock %.3f  mode2 %.3f\n", ks, k2);
  EXPECT_LT(ks, 0.9);
  EXPECT_NEAR(k2, 1.0, 0.03);
}

TEST(PuschMmse2, ModeTwoKappaNearOneAcrossCorrelationsAndRecoversSymbols)
{
  for (double corr : {0.0, 0.5, 0.9}) {
    static Case b;
    Build(b, corr, 600, 99, 23, kShift);
    c16_t *cb[kL] = {b.comp[0], b.comp[1]};
    nr_mmse_2layers_unbiased(cb, kBuf, kBuf, kRx, kL, b.maga, b.magb, b.magc, b.chf, 8, kShift, 0, kRe);
    EXPECT_NEAR(Kappa(b), 1.0, 0.04) << "corr " << corr;
    /* noiseless: every equalised coordinate rounds to the transmitted one (threshold n1 = 8 coordinate units) */
    int bad = 0;
    for (int l = 0; l < kL; l++)
      for (int k = 0; k < kRe; k++) {
        const double u = b.maga[l][k].r / 8.0; // one coordinate unit in output scale
        if (u <= 0) { bad++; continue; }
        if (std::lround(b.comp[l][k].r / u) != b.sym[l][k][0] && std::abs(b.comp[l][k].r / u - b.sym[l][k][0]) > 1.0) bad++;
      }
    EXPECT_LE(bad, kRe / 100) << "corr " << corr;
  }
}

TEST(PuschMmse2, FlagOffPathIsTheUnchangedDeterministicStockFunction)
{
  static Case a, b;
  Build(a, 0.7, 600, 25, 5, kShift);
  memcpy(&b, &a, sizeof(a));
  c16_t *ca[kL] = {a.comp[0], a.comp[1]}, *cb[kL] = {b.comp[0], b.comp[1]};
  nr_mmse_2layers(ca, kBuf, kBuf, kRx, kL, a.maga, a.magb, a.magc, a.chf, kRb, 8, kShift, 0, kRe, 1000);
  nr_mmse_2layers(cb, kBuf, kBuf, kRx, kL, b.maga, b.magb, b.magc, b.chf, kRb, 8, kShift, 0, kRe, 1000);
  EXPECT_EQ(0, memcmp(&a, &b, sizeof(a)));
}

/* |H|^2 = 1e6, LS noise sigma^2 = 1e4, smoother gain g = 1/8: sig2 = 1e6 + 1250, resid2 = 8750 => SNR = 20 dB */
TEST(UlDmrsSnr, KnownNoise) {
  EXPECT_NEAR(20.0f, nr_ulsch_dmrs_snr_db(1e6 + 1250, 8750), 1e-3);
  EXPECT_TRUE(std::isnan(nr_ulsch_dmrs_snr_db(1e6, 0)));
  EXPECT_TRUE(std::isnan(nr_ulsch_dmrs_snr_db(0, 5)));
  EXPECT_NEAR(-30.0f, nr_ulsch_dmrs_snr_db(1.0, 1e4), 1e-3); /* signal below the noise floor clamps, never NaN/inf */
}
