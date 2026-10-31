/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* LINKLEVEL: controlled-signal BLER of the DL GPU pass chain against ideal references (test/benchmark, no product behaviour).
 *
 * One synthetic grant per trial (nr_gpu_pass_fixture.h: real OAI LDPC encoder, rate matching, scrambling, 4 RX, exact FEP -> chest -> demod on the GPU, int16 IQ ring) over a controlled
 * flat channel (chan_mode 1: every antenna gain 1 for one layer, orthogonal columns for two layers, so the post-combining SNR per layer is exactly noise_db + 10 log10(4)), AWGN.
 * The received samples (the GPU FEP output rxdataF) are then decoded by several receivers:
 *   A       our chain as shipped: GPU demod -> descramble -> nr_llr_norm_shift_nl (power-of-two shift) -> OAI rate dematch -> GPU LDPC (int8 min-sum, it_max 16) -> CRC
 *   A_cpu16 / A_cpu30   the same int8 LLRs through OAI's CPU LDPCdecoder (numMaxIter 16 / 30)
 *   AF      our GPU demod LLRs, descrambled, scaled by a float gain to mean |LLR| 40 (no power-of-two quantisation of the scale)
 *   j-2..j+2, j-3 ...   our chain with the normalised LLRs multiplied by 2^j before the int8 LDPC (j = -3..+2)
 *   IT      ideal LLR: exact log-likelihood ratio, true channel H, true noise variance, per-layer unbiased MMSE (= MRC for orthogonal columns), float gain to mean |LLR| 40
 *   ITml    same with the max-log approximation;  IT10/IT20/IT80: exact, gain to mean 10/20/80 (the int8 scale of the ideal LLRs)
 *   IE      exact LLR formula with OUR chest estimates (est_all) and the true noise variance (isolates the chest from the demod arithmetic)
 *   IT_cpu16 / IT_cpu30   ideal LLRs through the CPU LDPC decoder
 * Every verdict is per code block (no CB-failure abort) and per TB (all code blocks). Output: CSV lines R,... (one per cfg, SNR, variant) and S,... (LLR statistics of A, genie SNR).
 * usage: nr_linklevel_bench <cfg,cfg,..|all> [trial_scale] [snr_step] ; run from the build directory (./libpdsch_gpu.so, ./libldpc.so). */
#include "nr_gpu_pass_fixture.h"
#include <map>
#include <string>
#include <algorithm>
#include <cstring>
#include <sstream>
extern "C" {
#include "PHY/CODING/coding_defs.h"
#include "PHY/CODING/nrLDPC_decoder/nrLDPC_types.h"
}

typedef std::complex<double> cd;
typedef int32_t (*ldpc_dec_fn)(t_nrLDPC_dec_params *, int8_t *, uint8_t *, t_nrLDPC_time_stats *, decode_abort_t *);

static double pam(int qm, int code)
{
  const int h = qm / 2;
  const int b[4] = {code & 1, (code >> 1) & 1, (code >> 2) & 1, (code >> 3) & 1};
  double a;
  if (h == 1) a = 1;
  else if (h == 2) a = 2 - (1 - 2 * b[1]);
  else a = 4 - (1 - 2 * b[1]) * (2 - (1 - 2 * b[2]));
  a *= 1 - 2 * b[0];
  return a / std::sqrt(qm == 2 ? 2 : qm == 4 ? 10 : 42);
}

/* ------------------------------------------------------------------ ideal receiver (float) */
struct Genie {
  double sigma2 = 0, snr_meas_db = 0, scale_rx = 0, scale_est = 0, bicm_se = 0;
  std::vector<float> lam;     /* exact LLR (natural log, positive = bit 0) of the scrambled coded bits, true H, true sigma2 */
  std::vector<float> lam_ml;  /* max-log */
  std::vector<float> lam_est; /* exact with our chest estimates */
  double hd_scale[2][4] = {}, hd_res_db[2][4] = {}; /* [0] our chest, [1] float reference (fw > 0): per layer |c| and the residual after removing it, dB rel. |s H|^2 */
  cd s_fit = 0;
  double dly_samples = 0; /* RANK34: timing offset of the FEP output, from the LS phase slope of layer 0 (samples of the 4096 grid) */
  std::vector<float> lam_f;   /* exact with a float LS + boxcar-smoothed + linearly interpolated chest (fw pairs; RANK34 reference), true sigma2 */
};

static void tx_symbols(const GpuPassFixture &f, std::vector<cd> &x)
{
  const int len = 12 * f.nrb, ns = (int)f.data_syms.size();
  x.assign((size_t)ns * len * f.nl, cd(0, 0));
  size_t txk = 0;
  for (int si = 0; si < ns; si++)
    for (int i = 0; i < len; i++)
      for (int l = 0; l < f.nl; l++) {
        int ci = 0, cq = 0;
        for (int b = 0; b < f.qm; b++) {
          const int bit = f.tx_bits[txk++];
          if (b & 1) cq |= bit << (b / 2); else ci |= bit << (b / 2);
        }
        x[((size_t)si * len + i) * f.nl + l] = cd(pam(f.qm, ci), pam(f.qm, cq));
      }
}

/* per-bit exact / max-log LLR of a rail value z (real) with complex noise variance v, bit position j of the rail code */
static inline void rail_llr(int qm, int j, double z, double v, double *ex, double *ml)
{
  const int h = qm / 2;
  double m0 = 1e300, m1 = 1e300;
  double d2[8];
  for (int c = 0; c < (1 << h); c++) {
    const double d = z - pam(qm, c);
    d2[c] = d * d;
    if ((c >> j) & 1) m1 = std::min(m1, d2[c]); else m0 = std::min(m0, d2[c]);
  }
  const double m = std::min(m0, m1);
  double a0 = 0, a1 = 0;
  for (int c = 0; c < (1 << h); c++) {
    const double e = std::exp(-(d2[c] - m) / v);
    if ((c >> j) & 1) a1 += e; else a0 += e;
  }
  double l = std::log(std::max(a0, 1e-300) / std::max(a1, 1e-300));
  *ex = std::max(-1000.0, std::min(1000.0, l));
  *ml = (m1 - m0) / v;
}

/* inverse of an n x n complex matrix (Gauss-Jordan, partial pivoting); false if singular */
static bool cinv(cd A[4][4], int n, cd Ai[4][4])
{
  cd M[4][8];
  for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) { M[i][j] = A[i][j]; M[i][n + j] = i == j ? 1.0 : 0.0; }
  for (int c = 0; c < n; c++) {
    int pv = c;
    for (int r = c + 1; r < n; r++) if (std::abs(M[r][c]) > std::abs(M[pv][c])) pv = r;
    if (std::abs(M[pv][c]) < 1e-300) return false;
    if (pv != c) for (int j = 0; j < 2 * n; j++) std::swap(M[c][j], M[pv][j]);
    const cd d = M[c][c];
    for (int j = 0; j < 2 * n; j++) M[c][j] /= d;
    for (int r = 0; r < n; r++) if (r != c) { const cd f = M[r][c]; for (int j = 0; j < 2 * n; j++) M[r][j] -= f * M[c][j]; }
  }
  for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) Ai[i][j] = M[i][n + j];
  return true;
}
/* est_scale: the chest estimate is used in units of rxF (checked by the regression scale_est ~ 1) */
static void ideal_llrs(const GpuPassFixture &f, Genie *g, int fw = 0, double s2_mul = 1.0, double s2_abs = 0.0, bool only_est = false)
{
  const int len = 12 * f.nrb, ns = (int)f.data_syms.size(), na = f.nant_, nl = f.nl, qm = f.qm, N = GpuPassFixture::N, NSYM = GpuPassFixture::NSYM;
  std::vector<cd> x;
  tx_symbols(f, x);
  auto rx = [&](int a, int sy, int i) {
    const size_t k = ((size_t)(a * NSYM + sy)) * N + (size_t)(f.fco_ + 12 * (f.start_rb + f.bwp) + i) % N;
    return cd(f.rxF_ref[2 * k], f.rxF_ref[2 * k + 1]);
  };
  auto es = [&](int l, int a, int sy, int i) {
    const int dm = (sy < 7 || f.ndm < 2) ? f.dm_sym[0] : f.dm_sym[1];
    const size_t k = (((size_t)(l * na + a)) * NSYM + dm) * N + i;
    return cd(f.est_all[2 * k], f.est_all[2 * k + 1]);
  };
  auto ht = [&](int a, int l, int i) { return f.htrue[((size_t)a * nl + l) * len + i]; };
  /* RANK34 float chest reference (type 1 DM-RS, the fixture's grant at start_rb): LS per comb pair with the OCC despread, boxcar over fw pairs, linear interpolation to the RE */
  std::vector<cd> Hf;
  if (fw > 0) {
    const int np = len / 4, ndm = f.ndm;
    Hf.assign((size_t)ndm * nl * na * len, cd(0, 0));
    std::vector<cd> ls(np), sm(np);
    for (int d = 0; d < ndm; d++)
      for (int l = 0; l < nl; l++)
        for (int a = 0; a < na; a++) {
          const int dl = f.delta(l);
          for (int q = 0; q < np; q++) {
            cd acc = 0;
            for (int kp = 0; kp < 2; kp++) {
              const int16_t *pp = &f.pilot[d][l][2 * (f.ppr() * (q / 3) + 2 * (q % 3) + kp)];
              const int ire = 12 * (q / 3) + 4 * (q % 3) + 2 * kp + dl;
              const size_t kr = ((size_t)(a * NSYM + f.dm_sym[d])) * N + (size_t)(f.fco_ + 12 * (f.start_rb + f.bwp) + ire) % N;
              acc += cd(pp[0], pp[1]) / 23170.0 * cd(f.rxF_ref[2 * kr], f.rxF_ref[2 * kr + 1]);
            }
            ls[q] = acc / (2.0 * std::sqrt(2.0));
          }
          if (l == 0 && d == 0) {
            cd sl = 0;
            for (int q = 0; q + 1 < np; q++) if (q % 3 != 2) sl += std::conj(ls[q]) * ls[q + 1];
            g->dly_samples += std::arg(sl) * 4096.0 / (2 * M_PI * 4.0) / na;
          }
          for (int q = 0; q < np; q++) { /* boxcar of fw pairs, window shifted inside [0, np) at the edges */
            int lo = std::max(0, std::min(np - fw, q - (fw - 1) / 2));
            const int hi = std::min(np, lo + fw);
            cd su = 0;
            for (int t = lo; t < hi; t++) su += ls[t];
            sm[q] = su / (double)(hi - lo);
          }
          for (int i = 0; i < len; i++) { /* pair q sits at RE 12 (q/3) + 4 (q%3) + dl + 1 */
            const double pos = (i - dl - 1) / 4.0;
            const int q0 = std::max(0, std::min(np - 2, (int)std::floor(pos)));
            const double fr = std::max(0.0, std::min(1.0, pos - q0));
            Hf[(((size_t)d * nl + l) * na + a) * len + i] = sm[q0] * (1 - fr) + sm[q0 + 1] * fr;
          }
        }
  }
  /* genie fit y = s * sum_l H_l x_l + n */
  cd num = 0;
  double den = 0;
  for (int si = 0; si < ns; si++)
    for (int i = 0; i < len; i++)
      for (int a = 0; a < na; a++) {
        cd u = 0;
        for (int l = 0; l < nl; l++) u += ht(a, l, i) * x[((size_t)si * len + i) * nl + l];
        num += std::conj(u) * rx(a, f.data_syms[si], i);
        den += std::norm(u);
      }
  const cd s = num / den;
  g->s_fit = s;
  double res = 0;
  size_t cnt = 0;
  double hh = 0;
  cd enum_ = 0;
  double eden = 0;
  for (int si = 0; si < ns; si++)
    for (int i = 0; i < len; i++)
      for (int a = 0; a < na; a++) {
        cd u = 0;
        for (int l = 0; l < nl; l++) u += ht(a, l, i) * x[((size_t)si * len + i) * nl + l];
        res += std::norm(rx(a, f.data_syms[si], i) - s * u);
        cnt++;
      }
  for (int i = 0; i < len; i++)
    for (int a = 0; a < na; a++)
      for (int l = 0; l < nl; l++) {
        hh += std::norm(ht(a, l, i));
        const cd t = s * ht(a, l, i), e = es(l, a, f.data_syms[0], i);
        enum_ += std::conj(t) * e;
        eden += std::norm(t);
      }
  for (int v = 0; v < (fw > 0 ? 2 : 1); v++)
    for (int l = 0; l < nl; l++) {
      cd nu = 0;
      double de = 0;
      for (int i = 0; i < len; i++)
        for (int a = 0; a < na; a++) {
          const int sy = f.data_syms[0];
          const cd t = s * ht(a, l, i), e = v == 0 ? es(l, a, sy, i) : Hf[(((size_t)0 * nl + l) * na + a) * len + i];
          nu += std::conj(t) * e;
          de += std::norm(t);
        }
      const cd c = nu / de;
      double rs = 0;
      for (int i = 0; i < len; i++)
        for (int a = 0; a < na; a++) {
          const int sy = f.data_syms[0];
          const cd t = s * ht(a, l, i), e = v == 0 ? es(l, a, sy, i) : Hf[(((size_t)0 * nl + l) * na + a) * len + i];
          rs += std::norm(e - c * t);
        }
      g->hd_scale[v][l] = std::abs(c);
      g->hd_res_db[v][l] = 10 * std::log10(std::max(1e-12, rs / de));
    }
  g->sigma2 = res / (double)cnt;
  g->scale_rx = std::abs(s);
  g->scale_est = std::abs(enum_ / eden);
  g->snr_meas_db = 10 * std::log10(std::norm(s) * (hh / ((double)len * nl)) / g->sigma2); /* sum_a |h|^2 per layer RE */
  const size_t G = f.tb.G;
  g->lam.assign(G, 0.f);
  g->lam_ml.assign(G, 0.f);
  g->lam_est.assign(G, 0.f);
  g->lam_f.assign(fw > 0 ? G : 0, 0.f);
  for (int variant = only_est ? 1 : 0; variant < (only_est ? 2 : fw > 0 ? 3 : 2); variant++) { /* 0: true H, 1: our estimates, 2: float chest reference */
    const double sg2 = variant == 1 ? (s2_abs > 0 ? s2_abs : g->sigma2 * s2_mul) : g->sigma2; /* the regularisation: the true noise variance unless the est variant is probed */
    size_t o = 0;
    for (int si = 0; si < ns; si++) {
      const int sy = f.data_syms[si];
      for (int i = 0; i < len; i++, o += (size_t)nl * qm) {
        cd H[4][4], y[4];
        for (int a = 0; a < na; a++) {
          y[a] = rx(a, sy, i);
          for (int l = 0; l < nl; l++)
            H[a][l] = variant == 0 ? s * ht(a, l, i) : variant == 1 ? es(l, a, sy, i) : Hf[(((size_t)((sy < 7 || f.ndm < 2) ? 0 : 1) * nl + l) * na + a) * len + i];
        }
        /* A = H^H H + sigma2 I (nl x nl), W = A^-1 H^H, G = W H (nl <= 4) */
        cd A[4][4], Ai[4][4];
        for (int l = 0; l < nl; l++)
          for (int k = 0; k < nl; k++) {
            A[l][k] = 0;
            for (int a = 0; a < na; a++) A[l][k] += std::conj(H[a][l]) * H[a][k];
            if (l == k) A[l][k] += sg2;
          }
        if (!cinv(A, nl, Ai)) continue;
        cd W[4][4];
        for (int l = 0; l < nl; l++)
          for (int a = 0; a < na; a++) {
            W[l][a] = 0;
            for (int k = 0; k < nl; k++) W[l][a] += Ai[l][k] * std::conj(H[a][k]);
          }
        for (int l = 0; l < nl; l++) {
          cd z = 0, Gll = 0;
          double wn = 0, gi = 0;
          for (int a = 0; a < na; a++) { z += W[l][a] * y[a]; wn += std::norm(W[l][a]); }
          for (int k = 0; k < nl; k++) {
            cd gk = 0;
            for (int a = 0; a < na; a++) gk += W[l][a] * H[a][k];
            if (k == l) Gll = gk; else gi += std::norm(gk);
          }
          const double mu2 = std::norm(Gll);
          const cd zu = z / Gll;
          const double v = std::max(1e-12, (gi + g->sigma2 * wn) / mu2); /* the LLR variance always uses the TRUE noise: only the filter's regularisation is probed */
          for (int b = 0; b < qm; b++) {
            double ex, ml;
            rail_llr(qm, b / 2, (b & 1) ? zu.imag() : zu.real(), v, &ex, &ml);
            const size_t p = o + (size_t)l * qm + b;
            if (variant == 0) { g->lam[p] = (float)ex; g->lam_ml[p] = (float)ml; }
            else if (variant == 1) g->lam_est[p] = (float)ex;
            else g->lam_f[p] = (float)ex;
          }
        }
      }
    }
  }
  double info = 0;
  for (size_t p = 0; p < G; p++) {
    const double sgn = f.tx_bits[p] ? 1.0 : -1.0; /* positive LLR = bit 0 */
    info += 1.0 - std::log2(1.0 + std::exp(std::max(-50.0, std::min(50.0, sgn * g->lam[p]))));
  }
  g->bicm_se = qm * info / (double)G;
}

/* ------------------------------------------------------------------ helpers */
struct Acc {
  int tbn = 0, tbf = 0;
  long cbn = 0, cbf = 0;
};

static std::vector<int16_t> to_i16(const std::vector<float> &lam, const GpuPassFixture &f, double target_mean)
{
  const size_t G = f.tb.G;
  double m = 0;
  for (size_t i = 0; i < G; i++) m += std::fabs(lam[i]);
  m /= (double)G;
  const double gain = m > 0 ? target_mean / m : 1.0;
  std::vector<int16_t> v(G);
  for (size_t i = 0; i < G; i++) {
    double y = lam[i] * gain;
    if (f.tb.scr[i >> 5] >> (i & 31) & 1u) y = -y; /* descramble: the fixture's convention */
    v[i] = (int16_t)std::lround(std::max(-32767.0, std::min(32767.0, y)));
  }
  return v;
}

static bool cpu_decode(GpuPassFixture &f, ldpc_dec_fn dec, int its, std::vector<uint8_t> *ok)
{
  static t_nrLDPC_time_stats *ts = (t_nrLDPC_time_stats *)calloc(1, 1 << 16);
  static decode_abort_t ab;
  static bool ab_init = (init_abort(&ab), true);
  (void)ab_init;
  const int C = f.tb.C, Z = f.tb.Z, Kc = f.tb.BG == 2 ? 52 : 68;
  const int Kprime = lenWithCrc(C, f.tb.A), ctype = crcType(C, f.tb.A);
  ok->assign((size_t)C, 0);
  static int8_t inc[68 * 384 + 64];
  static uint8_t out[8448 / 8 + 64];
  bool all = true;
  for (int r = 0; r < C; r++) {
    memcpy(inc, f.last_lin.data() + (size_t)r * NR_GPU_LDPC_DEV_IN_STRIDE, (size_t)Kc * Z);
    t_nrLDPC_dec_params p = {};
    p.BG = (uint8_t)f.tb.BG; p.Z = (uint16_t)Z; p.R = (uint8_t)((uint32_t)r < f.tb.first_rE2 ? f.tb.R : f.tb.R2); p.numMaxIter = (uint8_t)its;
    p.Kprime = Kprime; p.outMode = nrLDPC_outMode_BIT; p.crc_type = ctype; p.check_crc = check_crc;
    set_abort(&ab, false);
    memset(out, 0, sizeof(out));
    dec(&p, inc, out, ts, &ab);
    (*ok)[(size_t)r] = check_crc(out, (uint32_t)Kprime, (uint8_t)ctype) ? 1 : 0;
    all = all && (*ok)[(size_t)r];
  }
  return all;
}


/* ------------------------------------------------------------------ RANK1COMB: rank-1 combining under unequal per-RX noise / a co-channel interferer
 * Variants on the SAME received samples (GPU FEP output rxF_ref, our chest est_all), all through the same GPU LDPC:
 *   M3   the product stage path with the former equal-noise combiner (demod job nvar_ant = 0), dj_mode 3, ALIGN 2
 *   W    the product stage path with the per-RX noise-weighted MRC (nvar_ant = the chest residual noise per RX)
 *   GE   genie MMSE-IRC: true H, true noise + interferer covariance per RE        GW  genie noise-weighted MRC: true H, true per-RX noise (interferer ignored)
 *   IT   the equal-noise genie (existing ideal_llrs: true H, one scalar sigma2)
 *   EW   float weighted MRC: our est H, per-RX noise from the DM-RS residual (diag)       EI  float MMSE-IRC: our est H, full 4x4 residual covariance (shrunk toward its diagonal)
 * The SNR axis is the noise-only optimal-combining Es/N0: noise_db + 10 log10(sum_a gain_a^2 10^(-off_a/10)) (+ nothing for the interferer). */
struct Resid {
  cd R[4][4];
  int n = 0;
};
static void dmrs_resid(const GpuPassFixture &f, Resid *o)
{
  const int len = 12 * f.nrb, na = f.nant_, N = GpuPassFixture::N, NSYM = GpuPassFixture::NSYM;
  for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++) o->R[a][b] = 0;
  o->n = 0;
  for (int d = 0; d < f.ndm; d++)
    for (int i = 0; i < len; i++) {
      if ((i % 12) & 1) continue; /* type 1, port 1000: even REs */
      const int16_t *pp = &f.pilot[d][0][2 * (f.ppr() * (i / 12) + (i % 12) / 2)];
      const cd rwf = std::conj(cd(pp[0], pp[1])) / 23170.0; /* r * wf */
      cd e[4];
      for (int a = 0; a < na; a++) {
        const size_t kr = ((size_t)(a * NSYM + f.dm_sym[d])) * N + (size_t)(f.fco_ + 12 * (f.start_rb + f.bwp) + i) % N;
        const cd y(f.rxF_ref[2 * kr], f.rxF_ref[2 * kr + 1]);
        const size_t ke = (((size_t)a) * NSYM + f.dm_sym[d]) * N + i;
        e[a] = y / (std::sqrt(2.0) * rwf) - cd(f.est_all[2 * ke], f.est_all[2 * ke + 1]);
      }
      for (int a = 0; a < na; a++) for (int b = 0; b < na; b++) o->R[a][b] += e[a] * std::conj(e[b]);
      o->n++;
    }
  for (int a = 0; a < na; a++) for (int b = 0; b < na; b++) o->R[a][b] /= std::max(1, o->n);
}
/* kind 0 GE, 1 GW, 2 EW, 3 EI (shrink), 4 IT-like (flat sigma2 = mean of the true per-RX variances) */
static void r1_llrs(const GpuPassFixture &f, const Genie &g, int kind, double shrink, std::vector<float> *lam, double *sdiag_db /* est per-RX residual rel. RX0 */)
{
  const int len = 12 * f.nrb, ns = (int)f.data_syms.size(), na = f.nant_, qm = f.qm, N = GpuPassFixture::N, NSYM = GpuPassFixture::NSYM;
  std::vector<cd> x;
  tx_symbols(f, x);
  const double s = g.scale_rx; /* |s|: rxF units per channel unit (the fit's phase is irrelevant for the true H up to a common rotation) */
  (void)s;
  Resid rr;
  dmrs_resid(f, &rr);
  if (sdiag_db) for (int a = 0; a < na; a++) sdiag_db[a] = 10 * std::log10(std::max(1e-9, rr.R[a][a].real() / std::max(1e-9, rr.R[0][0].real())));
  cd Rest[4][4];
  for (int a = 0; a < na; a++)
    for (int b = 0; b < na; b++) Rest[a][b] = (a == b || kind == 2) ? (a == b ? rr.R[a][a] : cd(0, 0)) : rr.R[a][b] * (1.0 - shrink);
  /* the genie's phase-aligned scale: fit s as ideal_llrs (complex) */
  cd num = 0;
  double den = 0;
  auto rx = [&](int a, int sy, int i) {
    const size_t k = ((size_t)(a * NSYM + sy)) * N + (size_t)(f.fco_ + 12 * (f.start_rb + f.bwp) + i) % N;
    return cd(f.rxF_ref[2 * k], f.rxF_ref[2 * k + 1]);
  };
  auto ht = [&](int a, int i) { return f.htrue[((size_t)a * f.nl) * len + i]; };
  for (int si = 0; si < ns; si++)
    for (int i = 0; i < len; i++)
      for (int a = 0; a < na; a++) {
        const cd u = ht(a, i) * x[((size_t)si * len + i) * f.nl];
        num += std::conj(u) * rx(a, f.data_syms[si], i);
        den += std::norm(u);
      }
  const cd sc = num / den;
  const double s2 = std::norm(sc);
  lam->assign(f.tb.G, 0.f);
  size_t o = 0;
  double vmean = 0;
  for (int a = 0; a < na; a++) vmean += 2 * f.rx_noise_sd[a] * f.rx_noise_sd[a] * s2;
  vmean /= na;
  cd Rflat[4][4], Rflat_i[4][4];
  bool flat_ok = false;
  for (int si = 0; si < ns; si++) {
    const int sy = f.data_syms[si];
    const int dm = (sy < 7 || f.ndm < 2) ? f.dm_sym[0] : f.dm_sym[1];
    for (int i = 0; i < len; i++, o += (size_t)qm) {
      cd H[4], y[4], R[4][4], Ri[4][4];
      for (int a = 0; a < na; a++) {
        y[a] = rx(a, sy, i);
        const size_t ke = (((size_t)a) * NSYM + dm) * N + i;
        H[a] = kind <= 1 || kind == 4 ? sc * ht(a, i) : cd(f.est_all[2 * ke], f.est_all[2 * ke + 1]);
      }
      bool ok;
      if (kind == 0 || kind == 1) {
        for (int a = 0; a < na; a++) for (int b = 0; b < na; b++) {
          R[a][b] = a == b ? cd(2 * f.rx_noise_sd[a] * f.rx_noise_sd[a] * s2, 0) : cd(0, 0);
          if (kind == 0 && f.intf_p > 0) R[a][b] += s2 * f.intf_h[(size_t)a * len + i] * std::conj(f.intf_h[(size_t)b * len + i]);
        }
        ok = cinv(R, na, Ri);
      } else if (kind == 4) {
        for (int a = 0; a < na; a++) for (int b = 0; b < na; b++) R[a][b] = a == b ? cd(vmean, 0) : cd(0, 0);
        ok = cinv(R, na, Ri);
      } else {
        if (!flat_ok) { for (int a = 0; a < na; a++) for (int b = 0; b < na; b++) Rflat[a][b] = Rest[a][b]; flat_ok = cinv(Rflat, na, Rflat_i); }
        for (int a = 0; a < na; a++) for (int b = 0; b < na; b++) Ri[a][b] = Rflat_i[a][b];
        ok = flat_ok;
      }
      if (!ok) continue;
      cd z = 0;
      double mu = 0;
      for (int a = 0; a < na; a++) {
        cd w = 0;
        for (int b = 0; b < na; b++) w += std::conj(H[b]) * Ri[b][a]; /* (h^H R^-1)_a */
        z += w * y[a];
        mu += (w * H[a]).real();
      }
      if (mu <= 1e-30) continue;
      const cd zu = z / mu;
      const double v = std::max(1e-12, 1.0 / mu);
      for (int b = 0; b < qm; b++) {
        double ex, ml;
        rail_llr(qm, b / 2, (b & 1) ? zu.imag() : zu.real(), v, &ex, &ml);
        (*lam)[o + b] = (float)ex;
      }
    }
  }
}

struct Scn {
  const char *name;
  int chan;
  double off[4], gain[4], sir;
};
static const Scn SCN[] = {
    {"eq_flat", 1, {0, 0, 0, 0}, {1, 1, 1, 1}, 1e9},
    {"eq_ray", 2, {0, 0, 0, 0}, {1, 1, 1, 1}, 1e9},
    {"un3_flat", 1, {0, 3, 6, 10}, {1, 1, 1, 1}, 1e9},
    {"un3_ray", 2, {0, 3, 6, 10}, {1, 1, 1, 1}, 1e9},
    {"un3_fsel", 3, {0, 3, 6, 10}, {1, 1, 1, 1}, 1e9},
    {"dead_ray", 2, {0, 0, 0, 0}, {1, 1, 0, 0}, 1e9},
    {"dead_flat", 1, {0, 0, 0, 0}, {1, 1, 1, 0}, 1e9},
    {"rx0p3_flat", 1, {3, 0, 0, 0}, {1, 1, 1, 1}, 1e9},
    {"sir10_flat", 1, {0, 0, 0, 0}, {1, 1, 1, 1}, 10},
    {"sir5_flat", 1, {0, 0, 0, 0}, {1, 1, 1, 1}, 5},
    {"sir0_flat", 1, {0, 0, 0, 0}, {1, 1, 1, 1}, 0},
    {"sir5_ray", 2, {0, 0, 0, 0}, {1, 1, 1, 1}, 5},
    {"sir5_fsel", 3, {0, 0, 0, 0}, {1, 1, 1, 1}, 5},
    {"sir5_un3_ray", 2, {0, 3, 6, 10}, {1, 1, 1, 1}, 5},
};

static int run_r1(GpuPassModule &m, int argc, char **argv)
{
  static const int r64[29] = {120, 157, 193, 251, 308, 379, 449, 526, 602, 679, 340, 378, 434, 490, 553, 616, 658, 438, 466, 517, 567, 616, 666, 719, 772, 822, 873, 910, 948};
  struct C1 { const char *nm; int mcs, nrb; };
  const C1 cf[] = {{"t2", 9, 2}, {"t4", 9, 4}, {"s9", 9, 20}, {"m9", 9, 100}, {"s16", 16, 10}, {"m16", 16, 100}, {"m22", 22, 100}};
  const std::string csel = argc > 2 ? argv[2] : "m16", ssel = argc > 3 ? argv[3] : "all";
  const double tscale = argc > 4 ? atof(argv[4]) : 1.0, lo_off = argc > 5 ? atof(argv[5]) : 0.5, hi_off = argc > 6 ? atof(argv[6]) : 9.0, step = argc > 7 ? atof(argv[7]) : 0.5;
  const double shrink = argc > 8 ? atof(argv[8]) : 0.3;
  GpuPassFixture::max_cb = 40;
  GpuPassFixture::no_abort = true;
  GpuPassFixture::ldpc_it = 16;
  GpuPassFixture::dj_mode = 3; /* the product default */
  GpuPassFixture::dj_align = 2;
  std::unique_ptr<GpuPassFixture::Pass> P(new GpuPassFixture::Pass);
  bool pass_ok = false;
  for (const C1 &c : cf) {
    if (("," + csel + ",").find("," + std::string(c.nm) + ",") == std::string::npos) continue;
    const int qm = c.mcs < 10 ? 2 : c.mcs < 17 ? 4 : 6, nl = 1;
    const int G = c.nrb * 12 * 11 * qm * nl;
    const double Rn = r64[c.mcs] / 1024.0;
    GpuPassFixture::force_A = ((int)(Rn * G) - 24) & ~7;
    nr_gpu_pass_shape_t sh = {(uint16_t)c.nrb, 13, (uint16_t)nl, (uint16_t)qm, 2, 1, 1};
    for (const Scn &sc : SCN) {
      if (ssel != "all" && ("," + ssel + ",").find("," + std::string(sc.name) + ",") == std::string::npos) continue;
      GpuPassFixture::chan_mode = sc.chan;
      for (int a = 0; a < 4; a++) { GpuPassFixture::rx_noise_off_db[a] = sc.off[a]; GpuPassFixture::rx_gain[a] = sc.gain[a]; }
      GpuPassFixture::intf_sir_db = sc.sir;
      double wsum = 0;
      for (int a = 0; a < 4; a++) wsum += sc.gain[a] * sc.gain[a] * std::pow(10.0, -sc.off[a] / 10);
      const double axis_off = 10 * std::log10(wsum); /* noise-only optimal-combining Es/N0 = noise_db + this */
      GpuPassFixture::noise_db = 20;
      GpuPassFixture f0 = GpuPassFixture::from_shape(sh, 1);
      if (!f0.tb_ok_built) { printf("CFG,%s,build failed\n", c.nm); continue; }
      if (!pass_ok) { if (!P->setup(m, f0, -1)) { fprintf(stderr, "Pass setup failed\n"); return 1; } pass_ok = true; }
      const double se = (double)(f0.tb.A + 24.0) * qm / G, shannon = 10 * std::log10(std::pow(2.0, se) - 1);
      printf("SCN,%s,%s,mcs=%d,qm=%d,nrb=%d,C=%d,shannon=%.2f,axis_off=%.2f,chan=%d,sir=%.1f\n", c.nm, sc.name, c.mcs, qm, c.nrb, f0.tb.C, shannon, axis_off, sc.chan, sc.sir);
      fflush(stdout);
      const int ntr = std::max(8, (int)((f0.tb.C > 8 ? 30 : 60) * tscale));
      int calm = 0;
      std::vector<std::string> keep = {"M3", "W", "GE", "GW"};
      for (double snr = shannon + lo_off; snr <= shannon + hi_off; snr += step) {
        std::map<std::string, Acc> acc;
        std::vector<std::string> order;
        double sres[4] = {0, 0, 0, 0}, cest = 0;
        int ncest = 0;
        auto note = [&](const std::string &nm, const std::vector<uint8_t> &segok, bool tb) {
          if (!acc.count(nm)) order.push_back(nm);
          Acc &a = acc[nm];
          a.tbn++; a.tbf += !tb;
          for (uint8_t v : segok) { a.cbn++; a.cbf += !v; }
        };
        int done = 0;
        for (int t = 0; t < ntr; t++) {
          GpuPassFixture::noise_db = snr - axis_off;
          GpuPassFixture fx = GpuPassFixture::from_shape(sh, 1000 * (int)(snr * 4 + 400) + t + 7 * c.mcs + 100000 * c.nrb + 37 * (int)(&sc - SCN));
          if (!fx.tb_ok_built) continue;
          const int C = fx.tb.C;
          std::vector<uint8_t> tbuf((size_t)fx.tb.A / 8 + 4096, 0), segok((size_t)C);
          auto verdict = [&](const nr_gpu_pass_result_t &r, std::vector<uint8_t> *v) {
            bool allok = true;
            v->assign((size_t)C, 0);
            for (int i = 0; i < C; i++) { (*v)[(size_t)i] = r.seg_ok[i]; allok = allok && r.seg_ok[i]; }
            return allok;
          };
          for (int pass = 0; pass < 2; pass++) {
            GpuPassFixture::nvant_off = pass == 0;
            nr_gpu_pass_result_t s{};
            s.tb = tbuf.data();
            if (fx.run_stage_path(&s) != 0) { fprintf(stderr, "run_stage_path failed\n"); break; }
            note(pass == 0 ? "M3" : "W", segok, verdict(s, &segok));
            if (pass == 1) { /* relative spread of the chest's per-RX noise: c = p * sum_a (nv_a / mean - 1)^2 / (n - 1) (p = PRBs); for equal noise c = p cv^2 */
              double mean = 0;
              for (int a = 0; a < 4; a++) mean += s.nvar_ant[a] / 4.0;
              double S = 0;
              for (int a = 0; a < 4; a++) S += std::pow(s.nvar_ant[a] / std::max(mean, 1e-9) - 1.0, 2);
              cest += c.nrb * S / 3.0;
              ncest++;
            }
          }
          GpuPassFixture::nvant_off = false;
          if (acc["W"].tbn != acc["M3"].tbn) continue;
          Genie g;
          ideal_llrs(fx, &g);
          auto dec = [&](const char *nm, const std::vector<float> &lam) {
            nr_gpu_pass_result_t r{};
            r.tb = tbuf.data();
            std::vector<int16_t> l16 = to_i16(lam, fx, 40.0);
            if (fx.decode_llr(m, *P, P->s, l16, &r) != 0) return;
            std::vector<uint8_t> v;
            const bool tb = verdict(r, &v);
            note(nm, v, tb);
          };
          std::vector<float> lam;
          double sd[4] = {0, 0, 0, 0};
          r1_llrs(fx, g, 0, 0, &lam, nullptr); dec("GE", lam);
          r1_llrs(fx, g, 1, 0, &lam, nullptr); dec("GW", lam);
          r1_llrs(fx, g, 4, 0, &lam, nullptr); dec("IT", lam);
          r1_llrs(fx, g, 2, 0, &lam, sd); dec("EW", lam);
          for (int a = 0; a < 4; a++) sres[a] += sd[a];
          r1_llrs(fx, g, 3, shrink, &lam, nullptr); dec("EI", lam);
          done++;
        }
        bool calm_now = true;
        for (const std::string &nm : order) {
          const Acc &a = acc[nm];
          printf("R1,%s,%s,%.2f,%s,%d,%d,%ld,%ld\n", c.nm, sc.name, snr, nm.c_str(), a.tbn, a.tbf, a.cbn, a.cbf);
          if (std::find(keep.begin(), keep.end(), nm) != keep.end() && a.tbf > a.tbn / 50) calm_now = false;
        }
        printf("RS,%s,%s,%.2f,est_rel_rx0_db=%.2f/%.2f/%.2f/%.2f,c_spread=%.3f\n", c.nm, sc.name, snr, sres[0] / std::max(1, done), sres[1] / std::max(1, done), sres[2] / std::max(1, done), sres[3] / std::max(1, done), cest / std::max(1, ncest));
        fflush(stdout);
        calm = calm_now ? calm + 1 : 0;
        if (calm >= 2) break;
      }
    }
  }
  return 0;
}




/* RANK34: does the FEP output at the DM-RS REs equal what the fixture's model predicts (s * sum_l H_l conj(pil_l) sqrt2)? ratio per comb RE kp of the pair (complex, LS over RX and pairs) */
static void pilot_check(const GpuPassFixture &f, const Genie &g, cd ratio[2], double *rel_err_db)
{
  const int len = 12 * f.nrb, na = f.nant_, nl = f.nl, N = GpuPassFixture::N, NSYM = GpuPassFixture::NSYM, np = len / 4;
  cd nu[2] = {0, 0};
  double de[2] = {0, 0};
  for (int a = 0; a < na; a++)
    for (int q = 0; q < np; q++)
      for (int kp = 0; kp < 2; kp++) {
        cd pred = 0;
        const int ire = 12 * (q / 3) + 4 * (q % 3) + 2 * kp; /* group 0 REs */
        for (int l = 0; l < nl; l++) {
          if (f.delta(l)) continue;
          const int16_t *pp = &f.pilot[0][l][2 * (f.ppr() * (q / 3) + 2 * (q % 3) + kp)];
          pred += g.s_fit * f.htrue[((size_t)a * nl + l) * len + ire] * std::conj(cd(pp[0], pp[1]) / 23170.0) * std::sqrt(2.0);
        }
        const size_t kr = ((size_t)(a * NSYM + f.dm_sym[0])) * N + (size_t)(f.fco_ + 12 * (f.start_rb + f.bwp) + ire) % N;
        const cd y(f.rxF_ref[2 * kr], f.rxF_ref[2 * kr + 1]);
        nu[kp] += std::conj(pred) * y;
        de[kp] += std::norm(pred);
      }
  double er = 0, tot = 0;
  for (int kp = 0; kp < 2; kp++) { ratio[kp] = nu[kp] / de[kp]; }
  for (int a = 0; a < na; a++)
    for (int q = 0; q < np; q++)
      for (int kp = 0; kp < 2; kp++) {
        cd pred = 0;
        const int ire = 12 * (q / 3) + 4 * (q % 3) + 2 * kp;
        for (int l = 0; l < nl; l++) {
          if (f.delta(l)) continue;
          const int16_t *pp = &f.pilot[0][l][2 * (f.ppr() * (q / 3) + 2 * (q % 3) + kp)];
          pred += g.s_fit * f.htrue[((size_t)a * nl + l) * len + ire] * std::conj(cd(pp[0], pp[1]) / 23170.0) * std::sqrt(2.0);
        }
        const size_t kr = ((size_t)(a * NSYM + f.dm_sym[0])) * N + (size_t)(f.fco_ + 12 * (f.start_rb + f.bwp) + ire) % N;
        const cd y(f.rxF_ref[2 * kr], f.rxF_ref[2 * kr + 1]);
        er += std::norm(y - ratio[kp] * pred);
        tot += std::norm(y);
      }
  *rel_err_db = 10 * std::log10(std::max(1e-12, er / tot));
}

/* RANK34: a double emulation of the product's mode-3 chain for 16QAM (nr_mmse3_re + nrd_llr_re_out), from OUR chest estimates and the FEP output:
 * z = round(x / [A^-1]_ll gain), mag = round(w 0.6325), LLR = (zr, zi, mag - |zr|, mag - |zi|). round_z = false keeps z, mag in double (no int16 rounding).
 * Returns the hard-decision (sign) error fraction vs the transmitted bits and the fraction of LLRs that differ from `ref` (the product's llr_pre) when ref != nullptr. */
static void emul_mode3(const GpuPassFixture &f, double sigma2, bool round_z, double gain_const, const std::vector<int16_t> *ref, double *ber, double *mism, std::vector<double> *layer_ber = nullptr)
{
  const int len = 12 * f.nrb, ns = (int)f.data_syms.size(), na = f.nant_, nl = f.nl, N = GpuPassFixture::N, NSYM = GpuPassFixture::NSYM;
  auto rx = [&](int a, int sy, int i) {
    const size_t k = ((size_t)(a * NSYM + sy)) * N + (size_t)(f.fco_ + 12 * (f.start_rb + f.bwp) + i) % N;
    return cd(f.rxF_ref[2 * k], f.rxF_ref[2 * k + 1]);
  };
  auto es = [&](int l, int a, int sy, int i) {
    const int dm = (sy < 7 || f.ndm < 2) ? f.dm_sym[0] : f.dm_sym[1];
    const size_t k = (((size_t)(l * na + a)) * NSYM + dm) * N + i;
    return cd(f.est_all[2 * k], f.est_all[2 * k + 1]);
  };
  long err = 0, cnt = 0, mm = 0;
  std::vector<double> le(4, 0.0);
  size_t o = 0;
  for (int si = 0; si < ns; si++) {
    const int sy = f.data_syms[si];
    /* the int16 inputs of the product: round the estimates and samples to integers (they are integers already) */
    double emax[4] = {0, 0, 0, 0};
    for (int i = 0; i < len; i++)
      for (int l = 0; l < nl; l++)
        for (int a = 0; a < na; a++) emax[l] += std::norm(es(l, a, sy, i));
    double gd = 0;
    for (int l = 0; l < nl; l++) gd = std::max(gd, emax[l]);
    gd /= len;
    const double gain = gd > 0 ? gain_const / gd : 1.0;
    for (int i = 0; i < len; i++, o += (size_t)nl * f.qm) {
      cd H[4][4], y[4], A[4][4], Ai[4][4], b[4];
      for (int a = 0; a < na; a++) {
        y[a] = rx(a, sy, i);
        for (int l = 0; l < nl; l++) H[a][l] = es(l, a, sy, i);
      }
      for (int l = 0; l < nl; l++) {
        b[l] = 0;
        for (int a = 0; a < na; a++) b[l] += std::conj(H[a][l]) * y[a];
        for (int k = 0; k < nl; k++) {
          A[l][k] = 0;
          for (int a = 0; a < na; a++) A[l][k] += std::conj(H[a][l]) * H[a][k];
          if (l == k) A[l][k] += sigma2;
        }
      }
      if (!cinv(A, nl, Ai)) continue;
      for (int l = 0; l < nl; l++) {
        cd x = 0;
        for (int k = 0; k < nl; k++) x += Ai[l][k] * b[k];
        const double ai = Ai[l][l].real();
        const double w = (1.0 - sigma2 * ai) / ai * gain;
        double zr = x.real() / ai * gain, zi = x.imag() / ai * gain, mg = w * 20724.0 / 32768.0;
        if (round_z) {
          auto q = [](double v) { return (double)std::max(-32768L, std::min(32767L, std::lround(v))); };
          zr = q(zr); zi = q(zi); mg = q(mg);
        }
        const double ll[4] = {zr, zi, mg - std::fabs(zr), mg - std::fabs(zi)};
        for (int bb = 0; bb < 4; bb++) {
          const size_t p = o + (size_t)l * f.qm + bb;
          const bool e = (ll[bb] < 0) != (f.tx_bits[p] != 0);
          err += e; le[l] += e; cnt++;
          if (ref) {
            const int r16 = (*ref)[p];
            mm += (r16 < 0) != (ll[bb] < 0);
          }
        }
      }
    }
  }
  *ber = (double)err / std::max<long>(1, cnt);
  *mism = (double)mm / std::max<long>(1, cnt);
  if (layer_ber) for (int l = 0; l < nl; l++) (*layer_ber)[l] = le[l] / ((double)cnt / nl);
}

/* ------------------------------------------------------------------ RANK34: multi-layer (Nl 2..4) MMSE receiver on controlled correlated / selective channels
 * Variants on the SAME received samples (GPU FEP output), all through the same GPU LDPC (it_max 16):
 *   A   the product stage path (GPU FEP -> chest -> MMSE demod qam_mode 3 -> descramble -> int shift -> rate dematch -> GPU LDPC -> CRC)
 *   AF  our demod LLRs (pre-descramble) with a float gain to mean |LLR| 40 instead of the power-of-two shift
 *   E   float per-layer unbiased MMSE with OUR chest estimates (est_all) and the true noise variance (isolates the demod arithmetic + LLR scaling from the chest)
 *   G   genie: float MMSE with the true H and the true noise variance (the channel-physics reference)
 * SNR axis: per-RX, per-layer Es/N0 (E|h|^2 = 1, unit-power symbols; noise_db). A-E = demod arithmetic / int16 / scaling loss, E-G = channel estimation loss, A-G = total receiver loss.
 * usage: nr_linklevel_bench ml <cfg,..> <scn,..> [trial_scale] [lo_db] [hi_db] [step] */
static int ML_FW[4] = {1, 3, 7, 15}; /* float-chest reference windows (pairs) */
struct MlCfg { const char *nm; int nl, nrb, qm, r1024; };
static const MlCfg MLC[] = {
    {"n3q4", 3, 48, 4, 490}, {"n4q4", 4, 48, 4, 490}, {"n2q4", 2, 48, 4, 490}, {"n1q4", 1, 48, 4, 490},
    {"n3q4b", 3, 96, 4, 490}, {"n4q4b", 4, 96, 4, 490}, {"n2q4b", 2, 96, 4, 490},
    {"n3q4lo", 3, 48, 4, 378}, {"n4q4lo", 4, 48, 4, 378}, {"n3q4hi", 3, 48, 4, 616}, {"n4q4hi", 4, 48, 4, 616},
    {"n3q2", 3, 48, 2, 490}, {"n4q2", 4, 48, 2, 490},
    {"n3q6", 3, 48, 6, 490}, {"n4q6", 4, 48, 6, 490},
    /* BIGTB (test-only): 2 layers 64QAM at table-2 MCS 13 / 16 rates, small and big TB, and 2-layer 256QAM */
    {"n2q6", 2, 48, 6, 567}, {"n2q6b", 2, 120, 6, 567}, {"n2q6h", 2, 48, 6, 719}, {"n2q6hb", 2, 120, 6, 719}, {"n2q6l", 2, 200, 6, 567}, {"n2q8", 2, 48, 8, 822}, {"n2q8b", 2, 100, 8, 822},
};
struct MlScn { const char *nm; int chan; double rx, tx; };
static const MlScn MLS[] = {
    {"ray", 2, 0, 0}, {"fsel", 3, 0, 0},
    {"c50", 4, 0.5, 0}, {"c80", 4, 0.8, 0}, {"c95", 4, 0.95, 0},
    {"fc50", 5, 0.5, 0}, {"fc80", 5, 0.8, 0}, {"fc95", 5, 0.95, 0},
    {"c80t50", 4, 0.8, 0.5}, {"c95t80", 4, 0.95, 0.8}, {"fc80t50", 5, 0.8, 0.5},
};
static int run_ml(GpuPassModule &m, int argc, char **argv)
{
  const std::string csel = argc > 2 ? argv[2] : "n3q4", ssel = argc > 3 ? argv[3] : "ray";
  const double tscale = argc > 4 ? atof(argv[4]) : 1.0, lo = argc > 5 ? atof(argv[5]) : 8.0, hi = argc > 6 ? atof(argv[6]) : 30.0, step = argc > 7 ? atof(argv[7]) : 1.0;
  GpuPassFixture::max_cb = 40;
  GpuPassFixture::no_abort = true;
  GpuPassFixture::ldpc_it = 16;
  GpuPassFixture::dj_mode = 3; /* the product default (rank >= 2 ignores the branch mode: qam_mode 3 combines all RX) */
  GpuPassFixture::dj_align = 2;
  if (getenv("RANK34_RING_GAIN")) GpuPassFixture::ring_gain = atof(getenv("RANK34_RING_GAIN")); /* bench-only: the int16 ring level of the fixture */
  const bool extra = getenv("RANK34_EXTRA") != nullptr; /* bench-only: the float-chest / regularisation / chain-emulation extras (slower) */
  std::unique_ptr<GpuPassFixture::Pass> P(new GpuPassFixture::Pass);
  bool pass_ok = false;
  int ci = 0;
  for (const MlCfg &c : MLC) {
    ci++;
    if (("," + csel + ",").find("," + std::string(c.nm) + ",") == std::string::npos) continue;
    const int G = c.nrb * 12 * 11 * c.qm * c.nl;
    const double Rn = c.r1024 / 1024.0;
    GpuPassFixture::force_A = ((int)(Rn * G) - 24) & ~7;
    nr_gpu_pass_shape_t sh = {(uint16_t)c.nrb, 13, (uint16_t)c.nl, (uint16_t)c.qm, 2, 1, 1};
    int si = 0;
    for (const MlScn &sc : MLS) {
      si++;
      if (("," + ssel + ",").find("," + std::string(sc.nm) + ",") == std::string::npos) continue;
      GpuPassFixture::chan_mode = sc.chan;
      GpuPassFixture::corr_rx = sc.rx;
      GpuPassFixture::corr_tx = sc.tx;
      for (int a = 0; a < 4; a++) { GpuPassFixture::rx_noise_off_db[a] = 0; GpuPassFixture::rx_gain[a] = 1; }
      GpuPassFixture::intf_sir_db = 1e9;
      GpuPassFixture::noise_db = 20;
      GpuPassFixture f0 = GpuPassFixture::from_shape(sh, 1);
      if (!f0.tb_ok_built) { printf("CFG,%s,build failed\n", c.nm); continue; }
      if (!pass_ok) { if (!P->setup(m, f0, -1)) { fprintf(stderr, "Pass setup failed\n"); return 1; } pass_ok = true; }
      const double se = (double)(f0.tb.A + 24.0) * c.qm / G * c.nl;
      printf("SCN,%s,%s,nl=%d,qm=%d,nrb=%d,R=%.3f,A=%d,C=%d,BG=%d,sum_bits_per_re_per_layer=%.3f,chan=%d,rx=%.2f,tx=%.2f\n", c.nm, sc.nm, c.nl, c.qm, c.nrb, Rn, f0.tb.A, f0.tb.C, f0.tb.BG, se / c.nl, sc.chan, sc.rx, sc.tx);
      fflush(stdout);
      const int ntr = std::max(8, (int)((f0.tb.C > 8 ? 40 : 60) * tscale));
      int calm = 0;
      for (double snr = lo; snr <= hi + 1e-9; snr += step) {
        std::map<std::string, Acc> acc;
        std::vector<std::string> order;
        double pc[4] = {0, 0, 0, 0};
        double dly_s = 0, rpk = 0;
        long rcl = 0;
        double hs[2][4] = {}, hr[2][4] = {};
        double em[5] = {0, 0, 0, 0, 0};
        double lbA[4] = {0, 0, 0, 0}, lbE[4] = {0, 0, 0, 0}, lbG[4] = {0, 0, 0, 0};
        double s_rat = 0, s_snr = 0, s_bg = 0, s_be = 0, s_ra = 0, s_rg = 0, s_re = 0, s_sce = 0, s_clip = 0;
        int done = 0;
        auto note = [&](const std::string &nm, const std::vector<uint8_t> &segok, bool tb) {
          if (!acc.count(nm)) order.push_back(nm);
          Acc &a = acc[nm];
          a.tbn++; a.tbf += !tb;
          for (uint8_t v : segok) { a.cbn++; a.cbf += !v; }
        };
        for (int t = 0; t < ntr; t++) {
          GpuPassFixture::noise_db = snr;
          GpuPassFixture fx = GpuPassFixture::from_shape(sh, 1000 * t + 17 * ci + 3 * si + 5);
          if (!fx.tb_ok_built) continue;
          const int C = fx.tb.C;
          std::vector<uint8_t> tbuf((size_t)fx.tb.A / 8 + 4096, 0), segok((size_t)C);
          auto verdict = [&](const nr_gpu_pass_result_t &r, std::vector<uint8_t> *v) {
            bool allok = true;
            v->assign((size_t)C, 0);
            for (int i = 0; i < C; i++) { (*v)[(size_t)i] = r.seg_ok[i]; allok = allok && r.seg_ok[i]; }
            return allok;
          };
          nr_gpu_pass_result_t s{};
          s.tb = tbuf.data();
          if (fx.run_stage_path(&s) != 0) { fprintf(stderr, "run_stage_path failed\n"); continue; }
          note("A", segok, verdict(s, &segok));
          size_t eA = 0, clip = 0;
          for (size_t i = 0; i < fx.tb.G; i++) { eA += (fx.llr_pre[i] < 0) != (fx.tx_bits[i] != 0); clip += std::abs((int)fx.llr_ref[i]) >= 127; }
          s_ra += (double)eA / fx.tb.G;
          s_clip += (double)clip / fx.tb.G;
          rpk = std::max(rpk, fx.ring_peak); rcl += fx.ring_clip;
          Genie g;
          ideal_llrs(fx, &g);
          s_snr += g.snr_meas_db; s_sce += g.scale_est;
          auto dec = [&](const char *nm, const std::vector<float> &lam) {
            nr_gpu_pass_result_t r{};
            r.tb = tbuf.data();
            std::vector<int16_t> l16 = to_i16(lam, fx, 40.0);
            if (fx.decode_llr(m, *P, P->s, l16, &r) != 0) return;
            std::vector<uint8_t> v;
            const bool tb = verdict(r, &v);
            note(nm, v, tb);
          };
          if (extra) for (int w = 0; w < 4; w++) { Genie gw; ideal_llrs(fx, &gw, ML_FW[w]); char nm[8]; snprintf(nm, sizeof(nm), "F%d", ML_FW[w]); dec(nm, gw.lam_f); if (w == 3) dly_s += gw.dly_samples; if (w == 3) for (int l = 0; l < 4; l++) for (int v = 0; v < 2; v++) { hs[v][l] += g.hd_scale[v == 0 ? 0 : 0][l] * (v == 0); hs[v][l] += gw.hd_scale[v][l] * (v == 1); hr[v][l] += gw.hd_res_db[v][l]; } }
          if (extra) { /* regularisation probe: the est-H float MMSE with s2 x0.25 / x4 and with the product's own s2 (chest nvar x nvfac; only meaningful for nl >= nrx) */
            Genie gq;
            ideal_llrs(fx, &gq, 0, 0.25, 0, true); dec("Em4", gq.lam_est);
            ideal_llrs(fx, &gq, 0, 4.0, 0, true); dec("Ep4", gq.lam_est);
            const double sprod = (double)s.nvar * std::pow(10.0, 2.1);
            s_rat += std::log10(std::max(1e-9, sprod / g.sigma2));
            ideal_llrs(fx, &gq, 0, 1.0, sprod, true); dec("Eprod", gq.lam_est);
          }
          {
            std::vector<float> lp(fx.tb.G);
            for (size_t i = 0; i < fx.tb.G; i++) lp[i] = (float)fx.llr_pre[i];
            dec("AF", lp);
          }
          dec("G", g.lam);
          dec("E", g.lam_est);
          size_t eg = 0, ee = 0;
          double bg = 0, be = 0;
          for (size_t i = 0; i < fx.tb.G; i++) {
            const bool b1 = fx.tx_bits[i] != 0;
            eg += (g.lam[i] < 0) != b1; ee += (g.lam_est[i] < 0) != b1;
            const double sg = b1 ? 1.0 : -1.0;
            bg += 1.0 - std::log2(1.0 + std::exp(std::max(-50.0, std::min(50.0, sg * g.lam[i]))));
            be += 1.0 - std::log2(1.0 + std::exp(std::max(-50.0, std::min(50.0, sg * g.lam_est[i]))));
          }
          s_rg += (double)eg / fx.tb.G; s_re += (double)ee / fx.tb.G;
          { /* per-layer raw BER (LLR order: symbol, RE, layer, bit) */
            double ea[4] = {0, 0, 0, 0}, ee2[4] = {0, 0, 0, 0}, eg2[4] = {0, 0, 0, 0};
            for (size_t i = 0; i < fx.tb.G; i++) {
              const int l = (int)((i / fx.qm) % fx.nl);
              const bool b1 = fx.tx_bits[i] != 0;
              ea[l] += (fx.llr_pre[i] < 0) != b1; ee2[l] += (g.lam_est[i] < 0) != b1; eg2[l] += (g.lam[i] < 0) != b1;
            }
            for (int l = 0; l < fx.nl; l++) { const double d = (double)fx.tb.G / fx.nl; lbA[l] += ea[l] / d; lbE[l] += ee2[l] / d; lbG[l] += eg2[l] / d; }
          }
          s_bg += bg / fx.tb.G; s_be += be / fx.tb.G;
          if (extra) { cd rr[2]; double re; pilot_check(fx, g, rr, &re); pc[0] += std::abs(rr[0]); pc[1] += std::abs(rr[1]); pc[2] += std::arg(rr[1] / rr[0]); pc[3] += re; }
          if (extra && fx.qm == 4) { /* chain emulation: ablate the int16 rounding of z / mag and the gain constant */
            const double sprod = (double)s.nvar * std::pow(10.0, 2.1);
            double b1, m1, b2, m2, b3, m3, b4, m4;
            emul_mode3(fx, sprod, true, 1024.0, &fx.llr_pre, &b1, &m1);
            emul_mode3(fx, sprod, false, 1024.0, &fx.llr_pre, &b2, &m2);
            emul_mode3(fx, g.sigma2, false, 1024.0, nullptr, &b3, &m3);
            emul_mode3(fx, sprod, true, 65536.0, nullptr, &b4, &m4);
            em[0] += b1; em[1] += m1; em[2] += b2; em[3] += b3; em[4] += b4;
          }
          done++;
        }
        const double n = std::max(1, done);
        for (const std::string &nm : order) {
          const Acc &a = acc[nm];
          printf("R,%s,%s,%.2f,%s,%d,%d,%ld,%ld\n", c.nm, sc.nm, snr, nm.c_str(), a.tbn, a.tbf, a.cbn, a.cbf);
        }
        printf("S,%s,%s,%.2f,n=%d,snr_meas=%.2f,est_vs_true=%.3f,rawBER_A=%.5f,rawBER_E=%.5f,rawBER_G=%.5f,bicm_E=%.4f,bicm_G=%.4f,A_clip=%.4f,log10_s2prod_over_true=%.3f\n", c.nm, sc.nm, snr, done, s_snr / n, s_sce / n, s_ra / n, s_re / n, s_rg / n,
               s_be / n, s_bg / n, s_clip / n, s_rat / n);
        printf("L,%s,%s,%.2f,A=%.4f/%.4f/%.4f/%.4f,E=%.4f/%.4f/%.4f/%.4f,G=%.4f/%.4f/%.4f/%.4f\n", c.nm, sc.nm, snr, lbA[0] / n, lbA[1] / n, lbA[2] / n, lbA[3] / n, lbE[0] / n, lbE[1] / n, lbE[2] / n, lbE[3] / n, lbG[0] / n, lbG[1] / n, lbG[2] / n, lbG[3] / n);
        printf("H,%s,%s,%.2f,scale_est=%.3f/%.3f/%.3f/%.3f,res_est_db=%.1f/%.1f/%.1f/%.1f,scale_F15=%.3f/%.3f/%.3f/%.3f,res_F15_db=%.1f/%.1f/%.1f/%.1f\n", c.nm, sc.nm, snr, hs[0][0] / n, hs[0][1] / n, hs[0][2] / n, hs[0][3] / n, hr[0][0] / n, hr[0][1] / n, hr[0][2] / n, hr[0][3] / n, hs[1][0] / n, hs[1][1] / n, hs[1][2] / n, hs[1][3] / n, hr[1][0] / n, hr[1][1] / n, hr[1][2] / n, hr[1][3] / n);
        printf("P,%s,%s,%.2f,pilot_ratio_re0=%.4f,pilot_ratio_re1=%.4f,phase_re1_vs_re0_rad=%.4f,pilot_model_resid_db=%.1f\n", c.nm, sc.nm, snr, pc[0] / n, pc[1] / n, pc[2] / n, pc[3] / n);
        printf("K,%s,%s,%.2f,ring_peak=%.0f,ring_clipped_samples=%ld,fep_timing_offset_samples=%.2f\n", c.nm, sc.nm, snr, rpk, rcl, dly_s / n);
        printf("M,%s,%s,%.2f,emu_round_ber=%.5f,emu_round_vs_prod_mismatch=%.5f,emu_float_ber=%.5f,emu_float_trueS2_ber=%.5f,emu_round_gain65536_ber=%.5f\n", c.nm, sc.nm, snr, em[0] / n, em[1] / n, em[2] / n, em[3] / n, em[4] / n);
        fflush(stdout);
        const Acc &ag = acc["G"];
        calm = (ag.tbn && ag.tbf <= ag.tbn / 50) ? calm + 1 : 0;
        if (calm >= 3) break;
      }
    }
  }
  return 0;
}

struct Cfg {
  std::string name;
  int mcs, qm, nrb, nl, r1024;
  bool single;
};

int main(int argc, char **argv)
{
  nr_gpu_mem_test_force_discrete(0);
  bool ok = false;
  GpuPassModule &m = GpuPassModule::get(&ok);
  if (!ok) { fprintf(stderr, "no GPU module\n"); return 77; }
  void *hl = dlopen("./libldpc.so", RTLD_NOW | RTLD_GLOBAL);
  ldpc_dec_fn cpu = hl ? (ldpc_dec_fn)dlsym(hl, "LDPCdecoder") : nullptr;
  if (!cpu) { fprintf(stderr, "no LDPCdecoder\n"); return 77; }
  if (argc > 1 && !strcmp(argv[1], "r1")) return run_r1(m, argc, argv);
  if (argc > 1 && !strcmp(argv[1], "ml")) return run_ml(m, argc, argv);
  static const int r64[29] = {120, 157, 193, 251, 308, 379, 449, 526, 602, 679, 340, 378, 434, 490, 553, 616, 658, 438, 466, 517, 567, 616, 666, 719, 772, 822, 873, 910, 948};
  auto mk = [&](const char *nm, int mcs, int nrb, int nl, bool single) {
    const int qm = mcs < 10 ? 2 : mcs < 17 ? 4 : 6;
    return Cfg{nm, mcs, qm, nrb, nl, r64[mcs], single};
  };
  std::vector<Cfg> all = {
      mk("s4", 4, 44, 1, true), mk("s9", 9, 20, 1, true), mk("s16", 16, 10, 1, true), mk("s22", 22, 7, 1, true), mk("s27", 27, 11, 1, true),
      mk("m4", 4, 250, 1, false), mk("m9", 9, 100, 1, false), mk("m16", 16, 100, 1, false), mk("m22", 22, 100, 1, false), mk("m27", 27, 100, 1, false),
      mk("L22", 22, 250, 1, false),
      mk("d9", 9, 50, 2, false), mk("d22", 22, 50, 2, false), mk("d16", 16, 50, 2, false),
      /* BIGTB (test-only): the OTA failure region -- 64QAM / 256QAM at the rates of table-2 MCS 12..19 / 20..24, small to large TB (C 3 .. 30), 1 and 2 layers.
       * {name, mcs (unused), qm, nrb, nl, R*1024, single}; qm is explicit here because mk() derives it from the 64QAM table. */
      Cfg{"q6s", 0, 6, 20, 1, 567, false}, Cfg{"q6m", 0, 6, 100, 1, 567, false}, Cfg{"q6l", 0, 6, 273, 1, 567, false},
      Cfg{"q6s2", 0, 6, 20, 2, 567, false}, Cfg{"q6m2", 0, 6, 100, 2, 567, false}, Cfg{"q6l2", 0, 6, 273, 2, 567, false},
      Cfg{"q6l2h", 0, 6, 273, 2, 719, false}, Cfg{"q6m2h", 0, 6, 100, 2, 719, false},
      Cfg{"q8m", 0, 8, 100, 1, 822, false}, Cfg{"q8l", 0, 8, 273, 1, 822, false}, Cfg{"q8m2", 0, 8, 100, 2, 822, false}, Cfg{"q8l2", 0, 8, 150, 2, 822, false},
  };
  std::string sel = argc > 1 ? argv[1] : "all";
  const double tscale = argc > 2 ? atof(argv[2]) : 1.0;
  const double step = argc > 3 ? atof(argv[3]) : 0.5;
  GpuPassFixture::max_cb = 40;
  GpuPassFixture::chan_mode = 1;
  GpuPassFixture::no_abort = true;
  GpuPassFixture::ldpc_it = 16;
  GpuPassFixture::dj_mode = argc > 6 ? atoi(argv[6]) : 2; /* default: MRC over all 4 branches */
  GpuPassFixture::dj_align = argc > 7 ? atoi(argv[7]) : 2;
  std::unique_ptr<GpuPassFixture::Pass> P(new GpuPassFixture::Pass);
  bool pass_ok = false;
  for (const Cfg &c : all) {
    if (sel != "all" && ("," + sel + ",").find("," + c.name + ",") == std::string::npos) continue;
    const int G = c.nrb * 12 * 11 * c.qm * c.nl;
    const double Rn = c.r1024 / 1024.0;
    int A = ((int)(Rn * G) - 24) & ~7;
    GpuPassFixture::force_A = A;
    nr_gpu_pass_shape_t sh = {(uint16_t)c.nrb, 13, (uint16_t)c.nl, (uint16_t)c.qm, 2, 1, 1};
    GpuPassFixture::noise_db = 20;
    GpuPassFixture f0 = GpuPassFixture::from_shape(sh, 1);
    if (!f0.tb_ok_built) { printf("CFG,%s,build failed\n", c.name.c_str()); continue; }
    const double se = (double)f0.tb.A * c.qm / G + 24.0 * c.qm / G; /* bits per layer per RE incl. the TB CRC */
    const double shannon = 10 * std::log10(std::pow(2.0, se) - 1);
    const double req = 10 * std::log10(std::pow(2.0, c.qm * Rn) - 1) + 1.5;
    printf("CFG,%s,mcs=%d,qm=%d,nrb=%d,nl=%d,A=%d,BG=%d,C=%d,Z=%d,K=%d,F=%d,G=%d,E=%u,Reff=%.4f,SE=%.4f,shannon_db=%.3f,required_snr_db=%.3f\n", c.name.c_str(), c.mcs, c.qm, c.nrb, c.nl, f0.tb.A,
           f0.tb.BG, f0.tb.C, f0.tb.Z, f0.tb.K, f0.tb.F, G, f0.tb.E, (f0.tb.A + 24.0) / G, se, shannon, req);
    fflush(stdout);
    if (!pass_ok) { if (!P->setup(m, f0, -1)) { fprintf(stderr, "Pass setup failed\n"); return 1; } pass_ok = true; }
    const int ntr = std::max(8, (int)((c.single ? 100 : (f0.tb.C > 8 ? 30 : 50)) * tscale));
    double snr = std::floor((shannon - 0.5) / step) * step;
    double snr_max = shannon + 7.0;
    if (argc > 5 && atof(argv[4]) > -90) { snr = atof(argv[4]); snr_max = atof(argv[5]); }
    int calm = 0;
    for (int pi = 0; snr <= snr_max; pi++, snr += step) {
      std::map<std::string, Acc> acc;
      std::vector<std::string> order;
      double s_k = 0, s_clip = 0, s_mean = 0, s_snr = 0, s_bicm = 0, s_sc = 0, s_ber = 0, s_bert = 0, s_sce = 0, s_nv = 0;
      auto note = [&](const std::string &nm, const std::vector<uint8_t> &segok, bool tb) {
        if (!acc.count(nm)) order.push_back(nm);
        Acc &a = acc[nm];
        a.tbn++; a.tbf += !tb;
        for (uint8_t v : segok) { a.cbn++; a.cbf += !v; }
      };
      for (int t = 0; t < ntr; t++) {
        GpuPassFixture::noise_db = snr - 10 * std::log10(4.0);
        GpuPassFixture fx = GpuPassFixture::from_shape(sh, 1000 * (pi + 1) + t + 7 * c.mcs + 100000 * c.nrb);
        if (!fx.tb_ok_built) continue;
        const int C = fx.tb.C;
        std::vector<uint8_t> tbuf((size_t)fx.tb.A / 8 + 4096, 0), segok((size_t)C);
        nr_gpu_pass_result_t s{};
        s.tb = tbuf.data();
        if (fx.run_stage_path(&s) != 0) { fprintf(stderr, "run_stage_path failed\n"); continue; }
        auto verdict = [&](const nr_gpu_pass_result_t &r, std::vector<uint8_t> *v) {
          bool allok = true;
          v->assign((size_t)C, 0);
          for (int i = 0; i < C; i++) { (*v)[(size_t)i] = r.seg_ok[i]; allok = allok && r.seg_ok[i]; }
          return allok;
        };
        bool tbok = verdict(s, &segok);
        note("A", segok, tbok);
        /* LLR statistics of A (the int16 LLRs after the shift, before the int8 clamp) */
        {
          const std::vector<int16_t> &l = fx.llr_ref;
          size_t clip = 0;
          double sm = 0;
          for (size_t i = 0; i < fx.tb.G; i++) { const int a = std::abs((int)l[i]); clip += a >= 127; sm += a; }
          s_clip += (double)clip / fx.tb.G;
          s_mean += sm / fx.tb.G;
          s_k += nr_llr_norm_shift_nl(fx.llr_post.data(), nr_llr_norm_span(fx.tb.G, (uint32_t)C), fx.nl, fx.qm);
          size_t err = 0, errt = 0;
          for (size_t i = 0; i < fx.tb.G; i++) err += (fx.llr_pre[i] < 0) != (fx.tx_bits[i] != 0);
          s_ber += (double)err / fx.tb.G;
          (void)errt;
        }
        std::vector<uint8_t> cok;
        cpu_decode(fx, cpu, 16, &cok);
        note("A_cpu16", cok, std::all_of(cok.begin(), cok.end(), [](uint8_t v) { return v; }));
        cpu_decode(fx, cpu, 30, &cok);
        note("A_cpu30", cok, std::all_of(cok.begin(), cok.end(), [](uint8_t v) { return v; }));
        auto gpu_dec = [&](const char *nm, std::vector<int16_t> l) {
          nr_gpu_pass_result_t r{};
          r.tb = tbuf.data();
          if (fx.decode_llr(m, *P, P->s, l, &r) != 0) return false;
          std::vector<uint8_t> v;
          const bool tb = verdict(r, &v);
          note(nm, v, tb);
          return true;
        };
        /* scale sweep: shifted LLRs times 2^j (j > 0 left shift of the shifted value, j < 0 additional right shift) */
        {
          const int k = nr_llr_norm_shift_nl(fx.llr_post.data(), nr_llr_norm_span(fx.tb.G, (uint32_t)C), fx.nl, fx.qm);
          for (int j = -3; j <= 2; j++) {
            if (j == 0) continue;
            std::vector<int16_t> l(fx.tb.G);
            for (size_t i = 0; i < fx.tb.G; i++) {
              const int v = fx.llr_post[i];
              const int w = j >= 0 ? (v >> k) * (1 << j) : (v >> (k - j));
              l[i] = (int16_t)std::max(-32767, std::min(32767, w));
            }
            char nm[16];
            snprintf(nm, sizeof(nm), "j%+d", j);
            gpu_dec(nm, l);
          }
        }
        {
          std::vector<float> lp(fx.tb.G);
          for (size_t i = 0; i < fx.tb.G; i++) lp[i] = (float)fx.llr_pre[i]; /* pre-descramble, sign = tx bit convention */
          gpu_dec("AF", to_i16(lp, fx, 40.0));
        }
        Genie g;
        ideal_llrs(fx, &g);
        s_snr += g.snr_meas_db; s_bicm += g.bicm_se; s_sc += g.scale_rx; s_sce += g.scale_est; s_nv += g.sigma2;
        {
          size_t err = 0;
          for (size_t i = 0; i < fx.tb.G; i++) err += (g.lam[i] < 0) != (fx.tx_bits[i] != 0);
          s_bert += (double)err / fx.tb.G;
        }
        gpu_dec("IT", to_i16(g.lam, fx, 40.0));
        cpu_decode(fx, cpu, 16, &cok);
        note("IT_cpu16", cok, std::all_of(cok.begin(), cok.end(), [](uint8_t v) { return v; }));
        cpu_decode(fx, cpu, 30, &cok);
        note("IT_cpu30", cok, std::all_of(cok.begin(), cok.end(), [](uint8_t v) { return v; }));
        gpu_dec("ITml", to_i16(g.lam_ml, fx, 40.0));
        gpu_dec("IT10", to_i16(g.lam, fx, 10.0));
        gpu_dec("IT20", to_i16(g.lam, fx, 20.0));
        gpu_dec("IT80", to_i16(g.lam, fx, 80.0));
        gpu_dec("IE", to_i16(g.lam_est, fx, 40.0));
      }
      const double n = (double)acc["A"].tbn;
      printf("S,%s,%.2f,n=%d,A_k=%.2f,A_clip=%.4f,A_meanabs=%.2f,snr_meas=%.3f,bicm_se=%.4f,rx_scale=%.1f,est_vs_true=%.3f,sigma2=%.1f,rawBER_A=%.5f,rawBER_IT=%.5f\n", c.name.c_str(), snr, (int)n, s_k / n,
             s_clip / n, s_mean / n, s_snr / n, s_bicm / n, s_sc / n, s_sce / n, s_nv / n, s_ber / n, s_bert / n);
      bool calm_now = true;
      for (const std::string &nm : order) {
        const Acc &a = acc[nm];
        printf("R,%s,%.2f,%s,%d,%d,%ld,%ld\n", c.name.c_str(), snr, nm.c_str(), a.tbn, a.tbf, a.cbn, a.cbf);
        if ((nm == "A" || nm == "IT" || nm == "A_cpu16" || nm == "IE" || nm == "AF") && a.tbf > a.tbn / 50) calm_now = false;
      }
      fflush(stdout);
      calm = calm_now ? calm + 1 : 0;
      if (calm >= 2) break;
    }
  }
  return 0;
}
