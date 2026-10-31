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
