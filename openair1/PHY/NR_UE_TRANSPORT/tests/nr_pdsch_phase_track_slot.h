/* SPDX-License-Identifier: LicenseRef-CSSL-1.0
 * PHASE-TRACK tests: the synthetic slot (DM-RS on symbols 2 and 11, 4 RX, Nl layers, QAM data, a constant CFO plus per-branch LO drift) shared by the CPU and the GPU-twin test. */
#ifndef NR_PDSCH_PHASE_TRACK_SLOT_H
#define NR_PDSCH_PHASE_TRACK_SLOT_H
#include <cmath>
#include <complex>
#include <cstdint>
#include <random>
#include <vector>
extern "C" {
#include "nr_pdsch_phase_track.h"
}
typedef std::complex<double> cd;
static std::mt19937_64 rng(20261031);
static double gauss() { static std::normal_distribution<double> n(0.0, 1.0); return n(rng); }
static cd cgauss() { return cd(gauss(), gauss()) * std::sqrt(0.5); }

static const int N = 2048, NSYM = 14, NCOL = 12 * 100, DM0 = 2, DM1 = 11;
static const double SYM_S = 1e-3 / 28.0; /* 30 kHz SCS: 35.7 us per symbol incl. CP */

static cd qam(int qm)
{
  const int m = 1 << (qm / 2);
  double lv[16] = {};
  double pw = 0;
  for (int i = 0; i < m; i++) { lv[i] = -(m - 1) + 2 * i; pw += lv[i] * lv[i]; }
  pw = 2 * pw / m;
  return cd(lv[rng() % m], lv[rng() % m]) / std::sqrt(pw);
}
static c16_t q16(cd v, double sc) { return c16_t{(int16_t)std::lround(v.real() * sc), (int16_t)std::lround(v.imag() * sc)}; }

struct Slot {
  int nl, nrx, qm;
  std::vector<std::vector<c16_t>> est;  /* [nl*nrx][N*14] */
  std::vector<std::vector<c16_t>> y;    /* [nrx][N*14] */
  std::vector<std::vector<cd>> s;       /* [nl][NSYM*NCOL] */
  std::vector<std::vector<cd>> H;       /* true channel at the first DM-RS symbol, [nl*nrx][NCOL] */
  double f_hz = 0, drift[4] = {0, 0, 0, 0}, dmrs_noise = 0;
};

/* f_hz: common CFO; drift_hz[a]: extra per-branch frequency (independent LOs); dmrs_snr_db / data_snr_db */
static Slot make(int nl, int nrx, int qm, double f_hz, const double *drift_hz, double dmrs_snr_db, double data_snr_db)
{
  Slot S;
  S.nl = nl; S.nrx = nrx; S.qm = qm;
  const double A = 6000.0; /* channel amplitude in LSB (rms per element) */
  std::vector<std::vector<cd>> &H = S.H;
  H.assign(nl * nrx, std::vector<cd>(NCOL));
  for (auto &r : H) for (auto &v : r) v = cgauss() * A;
  S.f_hz = f_hz;
  for (int a = 0; a < 4; a++) S.drift[a] = drift_hz ? drift_hz[a] : 0.0;
  S.est.assign(nl * nrx, std::vector<c16_t>((size_t)N * NSYM, c16_t{0, 0}));
  S.y.assign(nrx, std::vector<c16_t>((size_t)N * NSYM, c16_t{0, 0}));
  S.s.assign(nl, std::vector<cd>((size_t)NSYM * NCOL));
  const double nd = A * std::pow(10.0, -dmrs_snr_db / 20.0);
  S.dmrs_noise = nd;
  const double nda = A * std::pow(10.0, -data_snr_db / 20.0);
  auto phase = [&](int a, int m) { return 2 * M_PI * (f_hz + (drift_hz ? drift_hz[a] : 0.0)) * (m - DM0) * SYM_S + 0.3 * a; };
  for (int l = 0; l < nl; l++)
    for (int a = 0; a < nrx; a++)
      for (int m : {DM0, DM1})
        for (int k = 0; k < NCOL; k++)
          S.est[l * nrx + a][(size_t)N * m + k] = q16(H[l * nrx + a][k] * std::polar(1.0, phase(a, m)) + cgauss() * nd, 1.0);
  for (int m = 0; m < NSYM; m++)
    for (int k = 0; k < NCOL; k++) {
      for (int l = 0; l < nl; l++) S.s[l][(size_t)m * NCOL + k] = qam(qm);
      for (int a = 0; a < nrx; a++) {
        cd v = 0;
        for (int l = 0; l < nl; l++) v += H[l * nrx + a][k] * std::polar(1.0, phase(a, m)) * S.s[l][(size_t)m * NCOL + k];
        S.y[a][(size_t)N * m + k] = q16(v + cgauss() * nda, 1.0);
      }
    }
  return S;
}


#endif
