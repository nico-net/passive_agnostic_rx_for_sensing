/* SPDX-License-Identifier: LicenseRef-CSSL-1.0
 * PHASE-TRACK GPU twin: the kernels k_cpe (per-symbol CPE tracker) and k_tinterp (channel time interpolation) of libpdsch_gpu.so against the CPU functions of nr_pdsch_phase_track.c, bit for bit
 * (double instantiation). A synthetic CFO slot is run through nr_pdsch_gpu_demod_run (k_cpe: the extraction buffers after the kernels are read back) and nr_pdsch_gpu_demod_dev (k_tinterp:
 * the estimate planes are rewritten in place). Skipped (GTEST_SKIP) without the module or a device, and on the DISCRETE tier (the buffers here are managed memory). */
#include <gtest/gtest.h>
#include <dlfcn.h>
#include <cuda_runtime.h>
#include <cstring>
#include "nr_pdsch_gpu_demod.h"
#include "nr_pdsch_phase_track_slot.h"

namespace {
typedef int (*init_fn)(int);
typedef int (*run_fn)(nr_gpu_demod_job_t *);
typedef void (*rel_fn)(int);
typedef int (*dev_fn)(const nr_gpu_demod_job_t *, nr_gpu_buf_t *, void *);
init_fn g_init;
run_fn g_run;
rel_fn g_rel;
dev_fn g_dev;
const int EXT = (NCOL + 15) & ~15;

template <class T> T *mm(size_t n)
{
  T *p = nullptr;
  if (cudaMallocManaged((void **)&p, n * sizeof(T)) != cudaSuccess)
    return nullptr;
  memset(p, 0, n * sizeof(T));
  return p;
}

class PhaseTrackGpu : public testing::Test {
 protected:
  static bool ok;
  static void SetUpTestSuite()
  {
    void *plat = dlopen("./libnr_gpu_platform.so", RTLD_NOW | RTLD_GLOBAL);
    void *dl = plat ? dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL) : nullptr;
    if (!dl)
      return;
    g_init = (init_fn)dlsym(dl, "nr_pdsch_gpu_demod_init");
    g_run = (run_fn)dlsym(dl, "nr_pdsch_gpu_demod_run");
    g_rel = (rel_fn)dlsym(dl, "nr_pdsch_gpu_demod_release");
    g_dev = (dev_fn)dlsym(dl, "nr_pdsch_gpu_demod_dev");
    ok = g_init && g_run && g_rel && g_dev && g_init(4) == 0;
  }
  void SetUp() override
  {
    if (!ok)
      GTEST_SKIP() << "no ./libpdsch_gpu.so, no device, or the module is not ready (CPU tier)";
  }
};
bool PhaseTrackGpu::ok = false;

/* the job of a grant whose symbols 2..13 are demodulated, identity geometry, buffers in managed memory */
struct Bufs {
  int16_t *rx = nullptr, *est = nullptr, *gy = nullptr, *gh = nullptr, *comp = nullptr, *mag = nullptr, *magb = nullptr, *magr = nullptr, *llr = nullptr;
};
void make_job(nr_gpu_demod_job_t &j, Bufs &B, const Slot &S, const std::vector<std::vector<c16_t>> &est)
{
  const int nl = S.nl, nrx = S.nrx, qm = S.qm;
  memset(&j, 0, sizeof(j));
  B.rx = mm<int16_t>((size_t)nrx * NSYM * N * 2);
  B.est = mm<int16_t>((size_t)nl * nrx * NSYM * N * 2);
  B.gy = mm<int16_t>((size_t)NSYM * EXT * 2);
  B.gh = mm<int16_t>((size_t)NSYM * EXT * 2);
  const size_t arr = (size_t)NSYM * 4 * EXT * 2;
  B.comp = mm<int16_t>(arr); B.mag = mm<int16_t>(arr); B.magb = mm<int16_t>(arr); B.magr = mm<int16_t>(arr);
  B.llr = mm<int16_t>((size_t)NSYM * EXT * qm * nl);
  for (int a = 0; a < nrx; a++) memcpy(B.rx + (size_t)a * NSYM * N * 2, S.y[a].data(), (size_t)NSYM * N * 4);
  for (int r = 0; r < nl * nrx; r++) memcpy(B.est + (size_t)r * NSYM * N * 2, est[r].data(), (size_t)NSYM * N * 4);
  for (int m = 0; m < NSYM; m++)
    for (int i = 0; i < EXT; i++) {
      B.gy[((size_t)m * EXT + i) * 2] = (int16_t)i;       B.gy[((size_t)m * EXT + i) * 2 + 1] = i < NCOL ? 1 : 0;
      B.gh[((size_t)m * EXT + i) * 2] = (int16_t)i;       B.gh[((size_t)m * EXT + i) * 2 + 1] = (int16_t)(m + 1);
    }
  j.nbRx = nrx; j.nl = nl; j.qm = qm; j.N = N;
  j.start_symbol = 2; j.nb_symbols = 12; j.first_symbol = 3;
  for (int m = 2; m < NSYM; m++) j.sym_mask |= (uint16_t)(1u << m);
  j.rx_stride = NSYM * N; j.est_stride = NSYM * N; j.buf = EXT; j.ext = EXT;
  uint32_t off = 0;
  for (int m = 2; m < NSYM; m++) { j.nre[m] = NCOL; j.llr_off[m] = off; off += NCOL * qm * nl; }
  j.rxdataF = B.rx; j.est = B.est; j.geo_y = B.gy; j.geo_h = B.gh;
  j.comp = B.comp; j.mag = B.mag; j.magb = B.magb; j.magr = B.magr; j.llr = B.llr;
  j.mode = 3; j.thr_coef = std::pow(10.0, -1.2); j.l2_delta = 0; j.align = 2;
  j.qam_mode = nl >= 3 ? 3 : 2;
  j.m3_nvfac = std::pow(10.0, 2.1);
  j.nvar = 1000;
  j.mag_per_layer = nl >= 2;
  j.want_kappa = 1; j.want_evm = 0;
  j.cpe = 1; j.tint = 0;
  j.tint_dm = (1u << DM0) | (1u << DM1);
}
} // namespace

/* k_cpe: the rotated channel rows of every data symbol equal nr_pdsch_cpe_symbol's, exactly */
TEST_F(PhaseTrackGpu, CpeKernelMatchesCpuBitExact)
{
  long rotated_total = 0, compared = 0;
  for (int nl : {2, 3, 4})
    for (double f : {0.0, 150.0, 400.0}) {
      const double drift[4] = {0.0, 60.0, -80.0, 30.0};
      Slot S = make(nl, 4, 6, f, drift, 30.0, 24.0);
      std::vector<c16_t *> rows(nl * 4);
      for (int r = 0; r < nl * 4; r++) rows[r] = S.est[r].data();
      ASSERT_EQ(nr_pdsch_phase_tinterp(rows.data(), nl, 4, N, NCOL, (1u << DM0) | (1u << DM1), 2, NSYM), 1);
      nr_gpu_demod_job_t j;
      Bufs B;
      make_job(j, B, S, S.est);
      ASSERT_EQ(g_run(&j), 0) << "nl=" << nl << " f=" << f;
      ASSERT_EQ(j.mrc_nb_rx, 4);
      ASSERT_NE(j.h_ext, nullptr);
      const c16_t *hx = (const c16_t *)j.h_ext;
      for (int m = 3; m < NSYM; m++) {
        if (m == DM1) continue;
        std::vector<std::vector<c16_t>> yv(4), hv(nl * 4);
        for (int a = 0; a < 4; a++) yv[a].assign(S.y[a].begin() + (size_t)N * m, S.y[a].begin() + (size_t)N * m + EXT);
        for (int r = 0; r < nl * 4; r++) hv[r].assign(S.est[r].begin() + (size_t)N * m, S.est[r].begin() + (size_t)N * m + EXT);
        c16_t *y[4] = {yv[0].data(), yv[1].data(), yv[2].data(), yv[3].data()};
        c16_t *h[4][4] = {};
        for (int l = 0; l < nl; l++) for (int a = 0; a < 4; a++) h[l][a] = hv[l * 4 + a].data();
        double phi[4];
        rotated_total += nr_pdsch_cpe_symbol(nl, 0, 4, NCOL, y, h, phi);
        for (int l = 0; l < nl; l++)
          for (int a = 0; a < 4; a++)
            for (int i = 0; i < NCOL; i++) {
              const c16_t g = hx[((size_t)m * 16 + l * 4 + a) * NR_GPU_DEMOD_MAX_EXT + i];
              const c16_t c = hv[l * 4 + a][i];
              compared++;
              if (g.r != c.r || g.i != c.i) {
                ADD_FAILURE() << "nl=" << nl << " f=" << f << " sym " << m << " layer " << l << " rx " << a << " re " << i << " gpu (" << g.r << "," << g.i << ") cpu (" << c.r << "," << c.i << ")";
                return;
              }
            }
        for (int l = 0; l < nl; l++) EXPECT_NEAR(j.cpe_phi[m][l], phi[l], 1e-5) << "counter nl=" << nl << " sym " << m;
      }
      g_rel(j.ctx);
    }
  printf("compared %ld channel values, %ld layer rotations applied\n", compared, rotated_total);
  EXPECT_GT(rotated_total, 20) << "the CFO slots must exercise the rotation";
}

/* k_tinterp: the data-symbol planes of the device estimate equal nr_pdsch_phase_tinterp's, exactly */
TEST_F(PhaseTrackGpu, TinterpKernelMatchesCpuBitExact)
{
  for (int nl : {1, 2, 4}) {
    const double drift[4] = {0.0, 60.0, -80.0, 30.0};
    Slot S = make(nl, 4, 6, 200.0, drift, 30.0, 30.0);
    std::vector<std::vector<c16_t>> ref = S.est;
    std::vector<c16_t *> rows(nl * 4);
    for (int r = 0; r < nl * 4; r++) rows[r] = ref[r].data();
    ASSERT_EQ(nr_pdsch_phase_tinterp(rows.data(), nl, 4, N, NCOL, (1u << DM0) | (1u << DM1), 2, NSYM), 1);
    nr_gpu_demod_job_t j;
    Bufs B;
    make_job(j, B, S, S.est); /* DM-RS planes only: the data planes are zero until the kernel writes them */
    j.nl = nl; j.tint = 1; j.tint_cols = NCOL;
    j.qam_mode = nl >= 3 ? 3 : 2;
    cudaStream_t st;
    ASSERT_EQ(cudaStreamCreate(&st), cudaSuccess);
    nr_gpu_buf_t llr = {};
    llr.dev = B.llr;
    ASSERT_EQ(g_dev(&j, &llr, st), 0);
    ASSERT_EQ(cudaStreamSynchronize(st), cudaSuccess);
    long bad = 0;
    for (int r = 0; r < nl * 4; r++)
      for (int m = 2; m < NSYM; m++) {
        if (m == DM0 || m == DM1) continue;
        const c16_t *g = (const c16_t *)(B.est + ((size_t)r * NSYM * N + (size_t)m * N) * 2);
        for (int k = 0; k < NCOL; k++)
          if (g[k].r != ref[r][(size_t)N * m + k].r || g[k].i != ref[r][(size_t)N * m + k].i) {
            if (bad++ < 5) ADD_FAILURE() << "nl=" << nl << " row " << r << " sym " << m << " col " << k << " gpu (" << g[k].r << "," << g[k].i << ") cpu (" << ref[r][(size_t)N * m + k].r << "," << ref[r][(size_t)N * m + k].i << ")";
          }
      }
    EXPECT_EQ(bad, 0) << "nl=" << nl;
    cudaStreamDestroy(st);
  }
}

/* ---- FP32 class vs the double (bit-exact) class, randomized grants -------------------------------------------------------------------------------------------------------------------
 * The production graph pass runs every PHASE-TRACK kernel (k_tinterp, k_cpe_*) and the IRC final stage in float (G.fp32 = nr_gpu_pass_fp32_default() = 1); the double instantiation stays
 * the CPU-exact reference. Same random 4-RX grant (Nl 1..4, 51..273 PRB, QPSK..256QAM, CFO + per-branch LO drift, 20..35 dB), run through both instantiations of the module.
 * Tolerances, from the quantisation of the chain:
 *  - the channel rows are int16 after the CPE rotation (nrpt_rot_re rounds): float and double may differ by one LSB where the rounding boundary lies between them, never by more
 *    (the float phasor error is ~1e-6 rad x |h| (<= 2^14) = 0.02 LSB);
 *  - the CPE angle is a ratio of sums over >= 48 REs, float rel. error ~1e-6 -> |dphi| <= 1e-4 rad (the rotation of a 2^13 channel by 1e-4 rad is 0.8 LSB);
 *  - the LLR is an int16 saturating function of z/thresholds that are themselves rounded to int16 (nr_mmse3_s16): a rounding-boundary flip of z or a threshold moves an LLR by its
 *    slope (<= 2^(Qm/2 - 1) LSB per LSB of z for 256QAM, 8 here), so |dLLR| <= 8 LSB for every LLR that is not saturated, with such flips on < 0.5 % of the LLRs;
 *  - a hard decision (LLR sign) may differ only where the double LLR is within the same bound of zero (the decision is then a coin flip of the rounding). */
namespace {
struct Rg {
  int nprb, ncol, nl, qm, N;
  std::vector<std::vector<int16_t>> est, y; /* est [nl*4][N*14*2], y [4][N*14*2] */
};
double g_u()
{
  static std::mt19937_64 r(20261101);
  return std::uniform_real_distribution<double>(0.0, 1.0)(r);
}
int g_pick(std::initializer_list<int> v)
{
  return *(v.begin() + (size_t)(g_u() * v.size()) % v.size());
}
Rg random_grant()
{
  static std::mt19937_64 gen(77);
  std::normal_distribution<double> nrm(0.0, std::sqrt(0.5));
  Rg g;
  g.nprb = g_pick({51, 79, 106, 160, 217, 273});
  g.nl = 1 + (int)(g_u() * 4) % 4;
  g.qm = g_pick({2, 4, 6, 8});
  g.ncol = 12 * g.nprb;
  g.N = 4096;
  const int nrx = 4, dm0 = 2, dm1 = 11;
  const double A = 6000.0, f_hz = (g_u() - 0.5) * 800.0, dmrs_db = 30.0, data_db = 20.0 + 15.0 * g_u(), sym_s = 1e-3 / 28.0;
  double drift[4];
  for (double &d : drift) d = (g_u() - 0.5) * 160.0;
  std::vector<std::complex<double>> H((size_t)g.nl * nrx * g.ncol);
  for (auto &v : H) v = std::complex<double>(nrm(gen), nrm(gen)) * A;
  g.est.assign((size_t)g.nl * nrx, std::vector<int16_t>((size_t)g.N * 14 * 2, 0));
  g.y.assign(nrx, std::vector<int16_t>((size_t)g.N * 14 * 2, 0));
  auto ph = [&](int a, int m) { return 2 * M_PI * (f_hz + drift[a]) * (m - dm0) * sym_s + 0.3 * a; };
  auto put = [](std::vector<int16_t> &v, size_t idx, std::complex<double> c) {
    v[2 * idx] = (int16_t)std::lround(std::max(-32767.0, std::min(32767.0, c.real())));
    v[2 * idx + 1] = (int16_t)std::lround(std::max(-32767.0, std::min(32767.0, c.imag())));
  };
  const double nd = A * std::pow(10.0, -dmrs_db / 20.0), nda = A * std::pow(10.0, -data_db / 20.0);
  for (int r = 0; r < g.nl * nrx; r++)
    for (int m : {dm0, dm1})
      for (int k = 0; k < g.ncol; k++)
        put(g.est[r], (size_t)m * g.N + k, H[(size_t)r * g.ncol + k] * std::polar(1.0, ph(r % nrx, m)) + std::complex<double>(nrm(gen), nrm(gen)) * nd);
  const int mq = 1 << (g.qm / 2);
  double pw = 0;
  for (int i = 0; i < mq; i++) pw += (2.0 * i - (mq - 1)) * (2.0 * i - (mq - 1));
  pw = 2 * pw / mq;
  for (int m = 0; m < 14; m++)
    for (int k = 0; k < g.ncol; k++) {
      std::complex<double> s[4];
      for (int l = 0; l < g.nl; l++)
        s[l] = std::complex<double>(2.0 * (int)(g_u() * mq) - (mq - 1), 2.0 * (int)(g_u() * mq) - (mq - 1)) / std::sqrt(pw);
      for (int a = 0; a < nrx; a++) {
        std::complex<double> v = 0;
        for (int l = 0; l < g.nl; l++) v += H[((size_t)l * nrx + a) * g.ncol + k] * std::polar(1.0, ph(a, m)) * s[l];
        put(g.y[a], (size_t)m * g.N + k, v + std::complex<double>(nrm(gen), nrm(gen)) * nda);
      }
    }
  return g;
}
struct Out {
  std::vector<int16_t> llr;
  std::vector<int16_t> h;       /* rotated channel rows (run) */
  float phi[14][4];
  std::vector<int16_t> est;     /* the interpolated estimate planes (dev) */
};
/* host-run path (k_cpe in the instantiation `f32`); `dev`: launch-only path with the interpolation (k_tinterp) */
void run_one(const Rg &g, int f32, bool dev, Out &o, int cpe = 1)
{
  const int ext = (g.ncol + 15) & ~15, nrx = 4;
  nr_gpu_demod_job_t j;
  memset(&j, 0, sizeof(j));
  Bufs B;
  B.rx = mm<int16_t>((size_t)nrx * 14 * g.N * 2);
  B.est = mm<int16_t>((size_t)g.nl * nrx * 14 * g.N * 2);
  B.gy = mm<int16_t>((size_t)14 * ext * 2);
  B.gh = mm<int16_t>((size_t)14 * ext * 2);
  const size_t arr = (size_t)14 * 4 * ext * 2;
  B.comp = mm<int16_t>(arr); B.mag = mm<int16_t>(arr); B.magb = mm<int16_t>(arr); B.magr = mm<int16_t>(arr);
  const size_t nllr = (size_t)12 * g.ncol * g.qm * g.nl;
  B.llr = mm<int16_t>(nllr);
  for (int a = 0; a < nrx; a++) memcpy(B.rx + (size_t)a * 14 * g.N * 2, g.y[a].data(), (size_t)14 * g.N * 4);
  for (int r = 0; r < g.nl * nrx; r++) memcpy(B.est + (size_t)r * 14 * g.N * 2, g.est[r].data(), (size_t)14 * g.N * 4);
  if (!dev) { /* the host-run path takes the interpolated estimate (the CPU does it ahead of the kernels) */
    std::vector<c16_t *> rows(g.nl * nrx);
    for (int r = 0; r < g.nl * nrx; r++) rows[r] = (c16_t *)(B.est + (size_t)r * 14 * g.N * 2);
    ASSERT_EQ(nr_pdsch_phase_tinterp(rows.data(), g.nl, nrx, g.N, g.ncol, (1u << 2) | (1u << 11), 2, 14), 1);
  }
  for (int m = 0; m < 14; m++)
    for (int i = 0; i < ext; i++) {
      B.gy[((size_t)m * ext + i) * 2] = (int16_t)i;  B.gy[((size_t)m * ext + i) * 2 + 1] = i < g.ncol ? 1 : 0;
      B.gh[((size_t)m * ext + i) * 2] = (int16_t)i;  B.gh[((size_t)m * ext + i) * 2 + 1] = (int16_t)(m + 1);
    }
  j.nbRx = nrx; j.nl = g.nl; j.qm = g.qm; j.N = g.N;
  j.start_symbol = 2; j.nb_symbols = 12; j.first_symbol = 3;
  for (int m = 2; m < 14; m++) j.sym_mask |= (uint16_t)(1u << m);
  j.rx_stride = 14 * g.N; j.est_stride = 14 * g.N; j.buf = ext; j.ext = ext;
  uint32_t off = 0;
  for (int m = 2; m < 14; m++) { j.nre[m] = g.ncol; j.llr_off[m] = off; off += g.ncol * g.qm * g.nl; }
  j.rxdataF = B.rx; j.est = B.est; j.geo_y = B.gy; j.geo_h = B.gh;
  j.comp = B.comp; j.mag = B.mag; j.magb = B.magb; j.magr = B.magr; j.llr = B.llr;
  j.mode = 3; j.thr_coef = std::pow(10.0, -1.2); j.align = 2;
  j.qam_mode = g.nl >= 2 ? 3 : 0;
  j.m3_nvfac = std::pow(10.0, 2.1);
  j.nvar = 1000;
  j.mag_per_layer = g.nl >= 2;
  j.want_kappa = 1;
  j.cpe = cpe; j.tint = dev ? 1 : 0; j.tint_cols = g.ncol;
  j.tint_dm = (1u << 2) | (1u << 11);
  j.demod_fp32 = f32;
  if (!dev) {
    ASSERT_EQ(g_run(&j), 0);
    o.h.assign((const int16_t *)j.h_ext, (const int16_t *)j.h_ext + (size_t)14 * 16 * NR_GPU_DEMOD_MAX_EXT * 2);
    memcpy(o.phi, j.cpe_phi, sizeof(o.phi));
    o.llr.assign(B.llr, B.llr + nllr);
    g_rel(j.ctx);
  } else {
    cudaStream_t st;
    ASSERT_EQ(cudaStreamCreate(&st), cudaSuccess);
    nr_gpu_buf_t lb = {};
    lb.dev = B.llr;
    ASSERT_EQ(g_dev(&j, &lb, st), 0);
    ASSERT_EQ(cudaStreamSynchronize(st), cudaSuccess);
    cudaStreamDestroy(st);
    o.est.assign(B.est, B.est + (size_t)g.nl * nrx * 14 * g.N * 2);
    o.llr.assign(B.llr, B.llr + nllr);
  }
  for (int16_t *p : {B.rx, B.est, B.gy, B.gh, B.comp, B.mag, B.magb, B.magr, B.llr}) cudaFree(p);
}
} // namespace

TEST_F(PhaseTrackGpu, Fp32ClassMatchesDoubleClassRandomGrants)
{
  struct Cnt { long n = 0, big = 0, sign = 0, sign_far = 0, diff = 0; int worst = 0; };
  auto cmp = [](Cnt &c, const Out &a, const Out &b) {
    for (size_t q = 0; q < a.llr.size(); q++) {
      const int x = a.llr[q], y = b.llr[q], dl = std::abs(x - y);
      c.n++;
      c.worst = std::max(c.worst, dl);
      c.big += dl > 8;
      c.diff += dl > 0;
      if ((x > 0 && y < 0) || (x < 0 && y > 0)) {
        c.sign++;
        c.sign_far += std::abs(x) > 8;
      }
    }
  };
  Cnt on, off, dv;
  long n_h = 0, n_h2 = 0, n_est = 0, n_est2 = 0, cpe_n = 0;
  double max_dphi = 0;
  const int trials = 48;
  for (int t = 0; t < trials; t++) {
    const Rg g = random_grant();
    Out d, f, dn, fn, di, fi;
    run_one(g, 0, false, d);
    run_one(g, 1, false, f);
    run_one(g, 0, false, dn, 0);
    run_one(g, 1, false, fn, 0);
    run_one(g, 0, true, di);
    run_one(g, 1, true, fi);
    if (HasFatalFailure()) return;
    /* CPE angle and rotated channel rows */
    for (int m = 3; m < 14; m++) {
      if (m == 11) continue;
      for (int l = 0; l < g.nl; l++) {
        max_dphi = std::max(max_dphi, (double)std::fabs(d.phi[m][l] - f.phi[m][l]));
        cpe_n += d.phi[m][l] != 0.f;
      }
      for (int r = 0; r < g.nl * 4; r++)
        for (int i = 0; i < g.ncol; i++)
          for (int c = 0; c < 2; c++) {
            const size_t ix = (((size_t)m * 16 + r) * NR_GPU_DEMOD_MAX_EXT + i) * 2 + c;
            n_h++;
            n_h2 += std::abs((int)d.h[ix] - (int)f.h[ix]) > 1;
          }
    }
    /* the interpolated estimate planes */
    for (int r = 0; r < g.nl * 4; r++)
      for (int m = 2; m < 14; m++)
        for (int i = 0; i < g.ncol; i++)
          for (int c = 0; c < 2; c++) {
            const size_t ix = (((size_t)r * 14 + m) * g.N + i) * 2 + c;
            n_est++;
            n_est2 += std::abs((int)di.est[ix] - (int)fi.est[ix]) > 1;
          }
    cmp(on, d, f);
    cmp(off, dn, fn);
    cmp(dv, di, fi);
  }
  printf("fp32 vs fp64, %d grants. PHASE-TRACK outputs: CPE %ld tracked layer-symbols, max |dphi| %.2e rad; rotated h %ld values, %ld off by >1 LSB; interpolated est %ld values, %ld off by >1 LSB\n"
         "  LLRs (whole FP32 class vs whole double class): tracker on %ld LLRs, %.3f %% differ, %ld sign diffs (%ld with |LLR|>8), max %d; tracker off: %.3f %% differ, %ld sign diffs, max %d; "
         "pass path (launch-only, with interpolation): %ld LLRs, %.4f %% differ, %ld sign diffs, %ld with |d|>8, max %d\n",
         trials, cpe_n, max_dphi, n_h, n_h2, n_est, n_est2, on.n, 100.0 * on.diff / on.n, on.sign, on.sign_far, on.worst, 100.0 * off.diff / off.n, off.sign, off.worst, dv.n, 100.0 * dv.diff / dv.n, dv.sign, dv.big, dv.worst);
  EXPECT_GT(cpe_n, 100) << "the CFO grants must exercise the tracker";
  EXPECT_LE(max_dphi, 1e-4);
  EXPECT_EQ(n_h2, 0);
  EXPECT_EQ(n_est2, 0);
  /* the LLR gate of the production FP32 pass (nr_gpu_pass_fp32_shadow): sign disagreement <= 0.1 %; the tracker must not add to what the float demod does by itself */
  EXPECT_LE(dv.sign, dv.n / 1000);
  EXPECT_LE(dv.big, dv.n / 1000) << "|dLLR| > 8 LSB on more than 0.1 % of the LLRs";
  EXPECT_LE(on.sign, off.sign + on.n / 100000) << "the float tracker added sign flips (> 0.001 %) on top of the float demod's";
}
