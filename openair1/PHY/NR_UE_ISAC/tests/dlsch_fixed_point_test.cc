/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 *
 * Fixed-point envelope of OAI's PDSCH 256QAM demodulation chain.
 *
 * WHY THIS EXISTS. The lab cell's wide (255-273 PRB) 256QAM MCS-25 grants decode 0 % while narrow
 * ones on the same cell decode 28-58 %, and the leading hypothesis was "the single output_shift,
 * derived from the MEAN |H|^2 over the allocation, starves the weak PRBs of LSBs". This test
 * MEASURES that hypothesis instead of arguing it: it drives OAI's OWN nr_channel_compensation()
 * and nr_256qam_llr() with an EXACT channel estimate, EXACT Y = H.x and NO noise, so every LLR
 * sign error it counts is fixed-point loss and nothing else.
 *
 * The three cases below are the measurement, not an illustration:
 *   - flat |H|, |H|rms 480              -> 0 errors with the production shift        (control)
 *   - 21 dB in-band tilt, |H|rms 480    -> 2.1 % errors with the production shift,
 *                                          0 with the peak-headroom shift            (the defect)
 *   - the lab cell's MEASURED |H| profile at its measured |H|rms 2672
 *                                       -> 0 errors BOTH ways                        (the refutation)
 * The last case is why ISAC_L2MAXH_HEADROOM defaults OFF and why the wide-grant failure is NOT
 * explained by this mechanism: at the level that receiver actually reports, the chain is exact.
 *
 * Build: cmake -DENABLE_TESTS=ON ; ninja/make test_dlsch_fixed_point ; ./test_dlsch_fixed_point
 */
#include <gtest/gtest.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <cstdint>
#include <algorithm>

/* Declared here rather than by including nr_phy_common.h / nr_channel_compensation.h: those headers
 * use C variably-modified array parameters (c16_t x[n][m]), which C++ cannot parse. Array parameters
 * decay to pointers at the ABI, so a void* declaration under extern "C" links to exactly the same
 * symbols and passes exactly the same arguments. c16_t is a pair of int16_t (PHY/impl_defs_top.h). */
struct c16_t {
  int16_t r, i;
};
extern "C" {
void nr_channel_compensation(uint32_t buffer_length,
                             uint32_t pdsch_buf_size_max,
                             int nb_rx_ant,
                             int nb_layers,
                             void *rxFext,
                             void *chFext,
                             void *ch_maga,
                             void *ch_magb,
                             void *ch_magc,
                             c16_t **rxComp,
                             void *rho,
                             int mod_order,
                             uint32_t symbol,
                             uint32_t output_shift);
void nr_256qam_llr(const c16_t *rxdataF_comp,
                   const c16_t *ch_mag,
                   const c16_t *ch_mag2,
                   const c16_t *ch_mag3,
                   int16_t *llr,
                   uint32_t nb_re);
int nr_log2_maxh_headroom(uint32_t peak_rb_h2, int contributing);
unsigned char log2_approx(unsigned int x);
}

/* LOG/CONFIG_LIB need these outside a full softmodem executable; same convention (and same reason)
 * as isac_sync_test.cc. Neither is reachable from the two kernels under test. */
struct configmodule_interface_s;
extern "C" struct configmodule_interface_s *uniqCfg = nullptr;
extern "C" void exit_function(const char *file, const char *function, const int line, const char *s, const int assert_)
{
  (void)file;
  (void)function;
  (void)line;
  (void)s;
  (void)assert_;
  abort();
}

namespace {

constexpr int kNbRe = 3276; // 273 PRB x 12
constexpr int kBufLen = 3280; // (N_RB_DL*12 + 15) & ~15, what nr_rx_pdsch() passes as rx_size_symbol

/* hrms_by_freq_bin from the failing OTA capture (EQDIAG, evmbase_205657, 273 PRB, 16 bins): this is
 * sqrt(mean|H|^2) per bin in the SAME raw int16 channel-estimate units nr_channel_level() averages. */
const double kOtaProfile[16] =
    {4610, 5609, 4604, 2852, 857, 2342, 2008, 2057, 1160, 1335, 1396, 491, 576, 727, 1853, 2842};

enum Profile { kFlat, kOta, kTilt };

struct Result {
  double ber_pct;      // LLR sign errors / total LLRs
  int shift_mean;      // nr_rx_pdsch()'s production rule
  int shift_headroom;  // nr_log2_maxh_headroom()
  int32_t peak_rb_h2;
};

// Deterministic LCG: the measurement must not move between runs.
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed ? seed : 1) {}
  uint32_t next() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return (uint32_t)(s >> 33); }
  double uni() { return (double)next() / 4294967296.0; }
};

/* One symbol of 273-PRB 256QAM through the production kernels. `shift_delta` lets a case ask for a
 * shift other than the production one; the default 0 is exactly what nr_rx_pdsch() would use. */
Result RunCase(Profile profile, double hrms, double spread_db, int use_headroom_shift)
{
  /* 64-byte aligned: nr_channel_compensation() dereferences these as simde__m256i*, i.e. ALIGNED
   * AVX2 loads. std::vector only guarantees 16 and faults here. */
  alignas(64) static c16_t rxf[kBufLen], chf[kBufLen], maga[kBufLen], magb[kBufLen], magc[kBufLen], rxcomp[kBufLen];
  alignas(64) static int16_t llr[8 * kBufLen];
  memset(rxf, 0, sizeof(rxf));
  memset(chf, 0, sizeof(chf));
  memset(maga, 0, sizeof(maga));
  memset(magb, 0, sizeof(magb));
  memset(magc, 0, sizeof(magc));
  memset(rxcomp, 0, sizeof(rxcomp));
  memset(llr, 0, sizeof(llr));

  std::vector<double> amp(kNbRe);
  double s2 = 0.0;
  for (int i = 0; i < kNbRe; i++) {
    double a;
    if (profile == kFlat)
      a = 1.0;
    else if (profile == kOta)
      a = kOtaProfile[std::min(15, i * 16 / kNbRe)];
    else
      a = std::pow(10.0, -(spread_db / 20.0) * ((double)i / (kNbRe - 1)));
    amp[i] = a;
    s2 += a * a;
  }
  const double norm = hrms / std::sqrt(s2 / kNbRe);
  for (int i = 0; i < kNbRe; i++)
    amp[i] *= norm;

  const double kR170 = std::sqrt(170.0);
  Rng rng(20260918u);
  std::vector<int> lr(kNbRe), li(kNbRe);
  int64_t sum_h2 = 0;
  for (int i = 0; i < kNbRe; i++) {
    const double ph = 2.0 * M_PI * rng.uni();
    chf[i].r = (int16_t)std::lround(amp[i] * std::cos(ph));
    chf[i].i = (int16_t)std::lround(amp[i] * std::sin(ph));
    const double hr = chf[i].r, hi = chf[i].i;
    sum_h2 += (int64_t)(hr * hr + hi * hi);
    lr[i] = 2 * (int)(rng.next() % 16) - 15; // odd levels -15..15
    li[i] = 2 * (int)(rng.next() % 16) - 15;
    const double xr = lr[i] / kR170, xi = li[i] / kR170;
    rxf[i].r = (int16_t)std::lround(hr * xr - hi * xi);
    rxf[i].i = (int16_t)std::lround(hr * xi + hi * xr);
  }

  Result out{};
  const int32_t avgs = (int32_t)(sum_h2 / kNbRe);
  // nr_rx_pdsch()'s single-layer rule, nr_dlsch_demodulation.c: (log2_approx(avgs)>>1) + 1 + log2(nbRx)
  out.shift_mean = (log2_approx((uint32_t)avgs) >> 1) + 1;
  // peak per-RB mean |H|^2, the same primitive nr_channel_level() uses
  int32_t peak = 0;
  for (int rb = 0; rb + 12 <= kNbRe; rb += 12) {
    int64_t acc = 0;
    for (int k = rb; k < rb + 12; k++)
      acc += (int64_t)chf[k].r * chf[k].r + (int64_t)chf[k].i * chf[k].i;
    const int32_t m = (int32_t)(acc / 12);
    if (m > peak)
      peak = m;
  }
  out.peak_rb_h2 = peak;
  out.shift_headroom = nr_log2_maxh_headroom((uint32_t)peak, 1);
  int shift = use_headroom_shift ? std::min(out.shift_mean, out.shift_headroom) : out.shift_mean;
  if (shift < 0)
    shift = 0;

  c16_t *rxcomp_p = rxcomp;
  nr_channel_compensation(kBufLen,
                          kBufLen,
                          1,
                          1,
                          rxf,
                          chf,
                          maga,
                          magb,
                          magc,
                          &rxcomp_p,
                          NULL,
                          8,
                          0,
                          (uint32_t)shift);
  nr_256qam_llr(rxcomp, maga, magb, magc, llr, kNbRe);

  long bad = 0, tot = 0;
  for (int i = 0; i < kNbRe; i++) {
    const int16_t *p = &llr[8 * i];
    for (int ax = 0; ax < 2; ax++) {
      const int L = ax ? li[i] : lr[i];
      int t[4];
      t[0] = L;
      t[1] = 8 - std::abs(L);
      t[2] = 4 - std::abs(t[1]);
      t[3] = 2 - std::abs(t[2]);
      for (int b = 0; b < 4; b++) {
        const int16_t v = p[2 * b + ax];
        if (v == 0 || ((v > 0) != (t[b] > 0)))
          bad++;
        tot++;
      }
    }
  }
  out.ber_pct = 100.0 * (double)bad / (double)tot;
  return out;
}

} // namespace

// Control: on a flat channel the production shift is exact. If this ever fails, the kernels or the
// harness changed, and none of the other cases can be read.
TEST(DlschFixedPoint, FlatChannelIsExactWithProductionShift)
{
  const Result r = RunCase(kFlat, 480.0, 0.0, 0);
  EXPECT_EQ(r.shift_mean, 10);
  EXPECT_DOUBLE_EQ(r.ber_pct, 0.0) << "flat |H|=480, no noise, exact H: any error here is a kernel bug";
}

// THE DEFECT, and the check that fails without the fix. 21 dB of in-band spread at |H|rms 480 (the
// level nr_dlsim reaches at -Q 36) puts the mean-derived shift outside the exact window; the
// headroom shift puts it back inside. The errors are POSITION-dependent: they are all in the weak
// end of the band, which is the OTA signature (crc_ok% collapsing once a TB covers the weak PRBs).
TEST(DlschFixedPoint, SpreadChannelNeedsTheHeadroomShift)
{
  const Result prod = RunCase(kTilt, 480.0, 21.0, 0);
  const Result fixed = RunCase(kTilt, 480.0, 21.0, 1);
  EXPECT_GT(prod.ber_pct, 1.0) << "expected the mean-derived shift to lose the weak PRBs";
  EXPECT_LT(fixed.shift_headroom, prod.shift_mean) << "headroom rule must keep MORE bits here";
  EXPECT_DOUBLE_EQ(fixed.ber_pct, 0.0) << "peak-sized shift must make the chain exact again";
}

// THE REFUTATION. At the |H| the failing OTA receiver actually reports (EQDIAG hrms 491..5609,
// band rms 2672) the production shift is ALREADY exact, and the headroom rule changes nothing.
// So the wide-grant 0 % is NOT this mechanism. Keep this assertion: it is what stops the headroom
// fix being sold as the OTA root cause, and it is what would have to break before it could be.
TEST(DlschFixedPoint, MeasuredOtaProfileIsExactEitherWay)
{
  const Result prod = RunCase(kOta, 2672.0, 0.0, 0);
  const Result fixed = RunCase(kOta, 2672.0, 0.0, 1);
  EXPECT_EQ(prod.shift_mean, 12);
  EXPECT_DOUBLE_EQ(prod.ber_pct, 0.0);
  EXPECT_DOUBLE_EQ(fixed.ber_pct, 0.0);
}

// The headroom rule must never keep FEWER bits than the production rule, at any level or spread --
// that is the property that makes it safe to enable on a receiver already inside the window.
TEST(DlschFixedPoint, HeadroomShiftNeverRaisesTheShift)
{
  for (double hrms : {200.0, 480.0, 1000.0, 2672.0, 6000.0}) {
    for (double sp : {0.0, 12.0, 21.0, 30.0}) {
      const Result prod = RunCase(kTilt, hrms, sp, 0);
      const Result fixed = RunCase(kTilt, hrms, sp, 1);
      EXPECT_LE(fixed.ber_pct, prod.ber_pct + 1e-9)
          << "hrms=" << hrms << " spread=" << sp << " prod_shift=" << prod.shift_mean
          << " hr_shift=" << fixed.shift_headroom;
    }
  }
}
