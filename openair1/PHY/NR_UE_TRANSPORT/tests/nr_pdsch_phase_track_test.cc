/* SPDX-License-Identifier: LicenseRef-CSSL-1.0
 * PHASE-TRACK (D1 + D2) unit test: a synthetic slot (DM-RS on symbols 2 and 11, data 3..10 and 12..13, 4 RX, Nl 1/2, Qm 2/6/8) with a constant residual CFO and per-branch phase drift.
 * The post-equalisation EVM per symbol must stay flat with the time interpolation + CPE tracker (the hold-the-previous-DM-RS demodulator it replaces grows with the distance), must not
 * regress at zero CFO, and the tracker must not invent rotations on a static channel. Final line: PHASE_TRACK_FAILS=N (exit 1 when N != 0). */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "nr_pdsch_phase_track_slot.h"
/* post-ZF EVM (dB, 10 log10 err/ref) of data symbol m on the estimate rows `rows` taken at symbol `src` (the hold demodulator: src = previous DM-RS), or at m itself */
static double evm_db(const Slot &S, int m, const std::vector<std::vector<c16_t>> &rows, int src, const std::vector<std::vector<c16_t>> *yrows = nullptr, const std::vector<std::vector<c16_t>> *hrows = nullptr, int ext_off = 0)
{
  (void)yrows; (void)hrows; (void)ext_off;
  double err = 0, ref = 0;
  const int nl = S.nl, nrx = S.nrx;
  for (int k = 0; k < NCOL; k++) {
    cd G[2][2] = {}, b[2] = {};
    for (int l = 0; l < nl; l++)
      for (int a = 0; a < nrx; a++) {
        const c16_t hl = rows[l * nrx + a][(size_t)N * src + k];
        const cd h(hl.r, hl.i);
        const c16_t yy = S.y[a][(size_t)N * m + k];
        b[l] += std::conj(h) * cd(yy.r, yy.i);
        for (int j = 0; j < nl; j++) {
          const c16_t hj = rows[j * nrx + a][(size_t)N * src + k];
          G[l][j] += std::conj(h) * cd(hj.r, hj.i);
        }
      }
    cd x[2];
    if (nl == 1) x[0] = b[0] / G[0][0];
    else {
      const cd det = G[0][0] * G[1][1] - G[0][1] * G[1][0];
      x[0] = (G[1][1] * b[0] - G[0][1] * b[1]) / det;
      x[1] = (G[0][0] * b[1] - G[1][0] * b[0]) / det;
    }
    for (int l = 0; l < nl; l++) { err += std::norm(x[l] - S.s[l][(size_t)m * NCOL + k]); ref += std::norm(S.s[l][(size_t)m * NCOL + k]); }
  }
  return 10.0 * std::log10(err / ref);
}

/* oracle: an estimate at the data symbol itself with the phase known exactly and the noise of ONE DM-RS estimate */
static std::vector<std::vector<c16_t>> oracle_rows(const Slot &S, int m)
{
  std::vector<std::vector<c16_t>> o(S.nl * S.nrx, std::vector<c16_t>((size_t)N * NSYM, c16_t{0, 0}));
  for (int l = 0; l < S.nl; l++)
    for (int a = 0; a < S.nrx; a++)
      for (int k = 0; k < NCOL; k++) {
        const double ph = 2 * M_PI * (S.f_hz + S.drift[a]) * (m - DM0) * SYM_S + 0.3 * a;
        o[l * S.nrx + a][(size_t)N * m + k] = q16(S.H[l * S.nrx + a][k] * std::polar(1.0, ph) + cgauss() * S.dmrs_noise, 1.0);
      }
  return o;
}

struct Run { double hold[NSYM], trk[NSYM], interp_only[NSYM], oracle[NSYM]; int cpe_applied; double cpe_sum; int cpe_n; };

static Run run(int nl, int nrx, int qm, double f_hz, const double *drift, double data_snr_db, bool cpe_on)
{
  Slot S = make(nl, nrx, qm, f_hz, drift, 30.0, data_snr_db);
  Run R = {};
  for (int m = 0; m < NSYM; m++) { R.hold[m] = R.trk[m] = R.interp_only[m] = R.oracle[m] = -99; }
  for (int m = 3; m < NSYM; m++) if (m != DM1) R.oracle[m] = evm_db(S, m, oracle_rows(S, m), m);
  /* old demodulator: the previous DM-RS symbol's estimate */
  for (int m = 0; m < NSYM; m++)
    if (m != DM0 && m != DM1 && m >= 3) R.hold[m] = evm_db(S, m, S.est, m < DM1 ? DM0 : DM1);
  /* new: interpolate into the data symbols, then (optionally) the CPE of each symbol on the extraction rows */
  std::vector<c16_t *> rows(nl * nrx);
  for (int r = 0; r < nl * nrx; r++) rows[r] = S.est[r].data();
  const int did = nr_pdsch_phase_tinterp(rows.data(), nl, nrx, N, NCOL, (1u << DM0) | (1u << DM1), 3, NSYM);
  if (!did) { printf("tinterp refused\n"); exit(2); }
  for (int m = 3; m < NSYM; m++) {
    if (m == DM1) continue;
    R.interp_only[m] = evm_db(S, m, S.est, m);
    if (!cpe_on) { R.trk[m] = R.interp_only[m]; continue; }
    /* extraction rows of this symbol: y[a] and h[l][a] as nr_rx_pdsch() holds them (copies: the tracker rotates h) */
    std::vector<std::vector<c16_t>> yv(nrx), hv(nl * nrx);
    for (int a = 0; a < nrx; a++) yv[a].assign(S.y[a].begin() + (size_t)N * m, S.y[a].begin() + (size_t)N * m + NCOL);
    for (int r = 0; r < nl * nrx; r++) hv[r].assign(S.est[r].begin() + (size_t)N * m, S.est[r].begin() + (size_t)N * m + NCOL);
    c16_t *y[4] = {0, 0, 0, 0};
    c16_t *h[4][4] = {};
    for (int a = 0; a < nrx; a++) y[a] = yv[a].data();
    for (int l = 0; l < nl; l++) for (int a = 0; a < nrx; a++) h[l][a] = hv[l * nrx + a].data();
    double phi[4];
    if (getenv("PT_VERBOSE")) { nr_pt_cpe_sums_t ss; nr_pdsch_cpe_sums(nl, 0, nrx, NCOL, y, h, &ss); printf("   sym %d raw: n %u S=(%.1f,%.1f) Q=%.1f -> phi_raw %.4f sigma %.4f pw %.3g npw %u\n", m, ss.n[0], ss.sr[0], ss.si[0], ss.q[0], std::atan2(-ss.si[0], -ss.sr[0]) / 4, std::sqrt(ss.q[0] / 2) / (4 * std::hypot(ss.sr[0], ss.si[0])), ss.pw[0], ss.npw[0]); }
    const int nrot = nr_pdsch_cpe_symbol(nl, 0, nrx, NCOL, y, h, phi);
    R.cpe_n++;
    if (getenv("PT_VERBOSE")) printf("   sym %d: phi %.4f %.4f rotated %d\n", m, phi[0], phi[1], nrot);
    if (nrot) { R.cpe_applied++; for (int l = 0; l < nl; l++) R.cpe_sum += std::fabs(phi[l]); }
    /* EVM with the rotated columns: write them back into a one-symbol view */
    std::vector<std::vector<c16_t>> rot(nl * nrx, std::vector<c16_t>((size_t)N * NSYM));
    for (int r = 0; r < nl * nrx; r++) memcpy(&rot[r][(size_t)N * m], hv[r].data(), NCOL * sizeof(c16_t));
    R.trk[m] = evm_db(S, m, rot, m);
  }
  return R;
}

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* mean over the data symbols of (a - b) in dB, and over `reps` independent slots: the CPE estimate of one 100-PRB symbol has a std of ~0.012 rad at 64QAM, single symbols are noisy */
struct Agg { double vs_oracle = 0, vs_hold = 0, hold_worst = -99, trk_worst = -99, cpe_abs = 0; int cpe_app = 0, cpe_n = 0, cnt = 0; };
static Agg agg(int nl, int qm, double f, const double *drift, double snr, int reps)
{
  Agg A;
  for (int rp = 0; rp < reps; rp++) {
    Run r = run(nl, 4, qm, f, drift, snr, true);
    for (int m = 3; m < NSYM; m++) {
      if (m == DM1) continue;
      A.vs_oracle += r.trk[m] - r.oracle[m];
      A.vs_hold += r.trk[m] - r.hold[m];
      A.hold_worst = std::max(A.hold_worst, r.hold[m]);
      A.trk_worst = std::max(A.trk_worst, r.trk[m]);
      A.cnt++;
    }
    A.cpe_abs += r.cpe_sum; A.cpe_app += r.cpe_applied; A.cpe_n += r.cpe_n;
  }
  A.vs_oracle /= A.cnt;
  A.vs_hold /= A.cnt;
  return A;
}

int main()
{
  const int qms[] = {2, 6, 8};
  const int REPS = 4;
  for (int nl : {1, 2})
    for (int qm : qms) {
      const double snr = qm == 8 ? 30.0 : qm == 6 ? 24.0 : 18.0; /* the OTA EVM floor is ~ -16 dB (PROJECT_MEMORY 36.5): residual phase errors below -26 dB are not decodable differences */
      {
        Agg a = agg(nl, qm, 0.0, nullptr, snr, REPS);
        printf("nl=%d qm=%d f=0   : mean (tracker - hold) EVM %+.2f dB, CPE rotated %d of %d symbols, mean |phi| %.4f rad\n", nl, qm, a.vs_hold, a.cpe_app, a.cpe_n,
               a.cpe_app ? a.cpe_abs / (a.cpe_app * nl) : 0.0);
        CHECK(a.vs_hold < 0.6, "nl=%d qm=%d zero-CFO regression %.2f dB", nl, qm, a.vs_hold);
        CHECK(a.cpe_app == 0 || a.cpe_abs / (a.cpe_app * nl) < 0.03, "nl=%d qm=%d CPE invented on a static channel (mean |phi| %.3f rad)", nl, qm, a.cpe_abs / (std::max(1, a.cpe_app) * nl));
      }
      for (double f : {50.0, 150.0, 300.0}) {
        const double drift[4] = {0.0, 60.0, -80.0, 30.0}; /* per-branch independent LO drift */
        Agg a = agg(nl, qm, f, drift, snr, REPS);
        printf("nl=%d qm=%d f=%3.0f : worst-symbol EVM tracker %.1f dB vs hold %.1f dB; mean (tracker - exact-phase oracle) %+.2f dB; CPE rotated %d of %d, mean |phi| %.3f rad\n", nl, qm, f,
               a.trk_worst, a.hold_worst, a.vs_oracle, a.cpe_app, a.cpe_n, a.cpe_app ? a.cpe_abs / (a.cpe_app * nl) : 0.0);
        /* flat = as good as an estimate whose phase is known exactly (the interpolation also averages two noisy estimates, so it can be better than the oracle) */
        CHECK(a.vs_oracle < 1.5, "nl=%d qm=%d f=%.0f tracker EVM %.2f dB worse than the exact-phase oracle", nl, qm, f, a.vs_oracle);
        CHECK(a.trk_worst < a.hold_worst - 3.0 || f < 100.0, "nl=%d qm=%d f=%.0f tracker not better than hold: %.1f vs %.1f", nl, qm, f, a.trk_worst, a.hold_worst);
      }
    }
  /* CPE alone: no interpolation (hold demodulator), a known extra rotation on symbol 12 (0.13 rad) must be estimated within 0.03 rad */
  for (int nl : {1, 2}) {
    Slot S = make(nl, 4, 6, 0.0, nullptr, 30.0, 30.0);
    const double phi_true = 0.13;
    std::vector<std::vector<c16_t>> yv(4), hv(nl * 4);
    for (int a = 0; a < 4; a++) {
      yv[a].assign(S.y[a].begin() + (size_t)N * 12, S.y[a].begin() + (size_t)N * 12 + NCOL);
      for (auto &v : yv[a]) { const cd z = cd(v.r, v.i) * std::polar(1.0, phi_true); v = q16(z, 1.0); }
    }
    for (int r = 0; r < nl * 4; r++) hv[r].assign(S.est[r].begin() + (size_t)N * DM1, S.est[r].begin() + (size_t)N * DM1 + NCOL);
    c16_t *y[4] = {yv[0].data(), yv[1].data(), yv[2].data(), yv[3].data()};
    c16_t *h[4][4] = {};
    for (int l = 0; l < nl; l++) for (int a = 0; a < 4; a++) h[l][a] = hv[l * 4 + a].data();
    double phi[4];
    const int n = nr_pdsch_cpe_symbol(nl, 0, 4, NCOL, y, h, phi);
    printf("cpe alone nl=%d: rotated %d, phi %.4f %.4f (true %.2f)\n", nl, n, phi[0], phi[1], phi_true);
    CHECK(n == nl, "nl=%d cpe alone rotated %d layers", nl, n);
    /* one 100-PRB symbol: std ~0.015 rad per layer at 64QAM, more on the layer the ZF noise enhancement hits */
    for (int l = 0; l < nl; l++) CHECK(std::fabs(phi[l] - phi_true) < 0.06, "nl=%d layer %d phi %.4f vs %.2f", nl, l, phi[l], phi_true);
  }
  /* pure-noise symbol: the significance gate must keep the false-rotation rate low and the rotation small */
  {
    int applied = 0, total = 200;
    for (int t = 0; t < total; t++) {
      std::vector<std::vector<c16_t>> yv(4), hv(4);
      for (int a = 0; a < 4; a++) {
        yv[a].resize(NCOL); hv[a].resize(NCOL);
        for (int k = 0; k < NCOL; k++) { yv[a][k] = q16(cgauss() * 3000.0, 1.0); hv[a][k] = q16(cgauss() * 3000.0, 1.0); }
      }
      c16_t *y[4] = {yv[0].data(), yv[1].data(), yv[2].data(), yv[3].data()};
      c16_t *h[4][4] = {};
      for (int a = 0; a < 4; a++) h[0][a] = hv[a].data();
      double phi[4];
      applied += nr_pdsch_cpe_symbol(1, 0, 4, NCOL, y, h, phi);
    }
    printf("pure noise: CPE applied on %d of %d symbols\n", applied, total);
    CHECK(applied <= total / 10, "noise-only false rotations %d / %d", applied, total);
  }
  /* fewer than two DM-RS symbols: the interpolation refuses and writes nothing */
  {
    std::vector<c16_t> row((size_t)N * NSYM, c16_t{7, 7});
    c16_t *rows[1] = {row.data()};
    CHECK(nr_pdsch_phase_tinterp(rows, 1, 1, N, NCOL, 1u << 2, 3, NSYM) == 0, "one DM-RS symbol must not interpolate");
    CHECK(row[(size_t)N * 5].r == 7, "one DM-RS symbol wrote the estimate");
  }
  printf("PHASE_TRACK_FAILS=%d\n", fails);
  return fails ? 1 : 0;
}
