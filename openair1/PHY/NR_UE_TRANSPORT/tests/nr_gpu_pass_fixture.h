/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* GPUPASS test fixture: one synthetic 4-RX, N = 4096 slot (own DM-RS and QAM, OAI TX symbol rotation, frequency-selective 4 x Nl
 * channel, AWGN, int16 IQ ring that wraps) pushed through the launch-only _dev entry points of libpdsch_gpu.so on ONE stream:
 * exact FEP -> exact chest -> demod -> (LDPC on CC >= 9). run_stages() allocates its device buffers under the current
 * ISAC_GPU_FORCE_DISCRETE, so the same fixture runs both memory models. Later tasks extend it (from_shape(...)). */
#ifndef NR_GPU_PASS_FIXTURE_H
#define NR_GPU_PASS_FIXTURE_H
#include <dlfcn.h>
#include <cuda_runtime.h>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
extern "C" {
#include "PHY/TOOLS/nr_gpu_mem.h"
}
#include "nr_pdsch_gpu_fep.h"
#include "nr_pdsch_gpu_chest.h"
#include "nr_pdsch_gpu_demod.h"
#include "nr_pdsch_gpu_ldpc.h"
#include "PHY/NR_UE_ESTIMATION/filt16a_32.h" /* the passive estimator's own UL filters */

struct GpuPassModule {
  void *dl = nullptr;
  nr_gpu_fep_dev_fn fep = nullptr;
  nr_gpu_chest_dev_fn chest = nullptr;
  nr_gpu_demod_dev_fn demod = nullptr;
  nr_gpu_ldpc_dev_fn ldpc = nullptr;
  /* the pre-GPUPASS entry points, with their own waits (DIRECT model only) */
  int (*fep_old)(const int16_t *const *, uint32_t, uint32_t, double, const int16_t *, uint16_t, int16_t *, size_t) = nullptr;
  int (*chest_old)(nr_gpu_chest_job_t *) = nullptr;
  int (*demod_old)(nr_gpu_demod_job_t *) = nullptr;
  void (*demod_rel)(int) = nullptr;
  int (*ldpc_old)(nr_gpu_ldpc_job_t *, int) = nullptr;
  /* loads ./libpdsch_gpu.so and inits FEP (N = 4096, 4 RX), demod and LDPC once; false = no module or no device */
  static GpuPassModule &get(bool *ok, bool *have_lib = nullptr)
  {
    static GpuPassModule m;
    static bool tried = false, good = false;
    if (!tried) {
      tried = true;
      m.dl = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
      if (m.dl) {
        auto api = (const nr_gpu_fep_api_t *(*)(void))dlsym(m.dl, "nr_gpu_fep_api");
        const nr_gpu_fep_cfg_t cfg = {4, 4096, 288, 352, 14, 4096 - 273 * 6, 273, 122880, 8};
        auto dinit = (nr_gpu_demod_init_fn)dlsym(m.dl, "nr_pdsch_gpu_demod_init");
        auto linit = (nr_gpu_ldpc_init_fn)dlsym(m.dl, "nr_pdsch_gpu_ldpc_init");
        m.fep = (nr_gpu_fep_dev_fn)dlsym(m.dl, "nr_pdsch_gpu_fep_dev");
        m.chest = (nr_gpu_chest_dev_fn)dlsym(m.dl, "nr_pdsch_gpu_chest_dev");
        m.demod = (nr_gpu_demod_dev_fn)dlsym(m.dl, "nr_pdsch_gpu_demod_dev");
        m.ldpc = (nr_gpu_ldpc_dev_fn)dlsym(m.dl, "nr_pdsch_gpu_ldpc_dev");
        m.fep_old = (decltype(m.fep_old))dlsym(m.dl, "nr_pdsch_gpu_exact_fep");
        m.chest_old = (decltype(m.chest_old))dlsym(m.dl, "nr_pdsch_gpu_exact_chest");
        m.demod_old = (decltype(m.demod_old))dlsym(m.dl, "nr_pdsch_gpu_demod_run");
        m.demod_rel = (decltype(m.demod_rel))dlsym(m.dl, "nr_pdsch_gpu_demod_release");
        m.ldpc_old = (decltype(m.ldpc_old))dlsym(m.dl, "nr_pdsch_gpu_ldpc_run");
        if (!m.fep) fprintf(stderr, "libpdsch_gpu.so has no nr_pdsch_gpu_fep_dev\n");
        good = api && m.fep_old && m.chest_old && m.demod_old && m.demod_rel && m.ldpc_old && dinit && linit && m.fep && m.chest && m.demod && m.ldpc && api()->init(&cfg) == 0 && dinit(4) == 0;
        if (good) linit(); /* may refuse (CC < 9): the LDPC stage then reports it */
      }
    }
    *ok = good;
    if (have_lib) *have_lib = m.dl != nullptr;
    return m;
  }
};

class GpuPassFixture {
 public:
  typedef std::complex<double> cd;
  static constexpr int N = 4096, NCP = 288, NCP0 = 352, NSYM = 14, NRB = 273, FCO = N - NRB * 6, NANT = 4;
  static constexpr int SLOT = NCP0 + N + (NSYM - 1) * (N + NCP);
  int nrb, nl, qm, start_rb = 10;
  int dm_sym[2] = {2, 11};
  std::vector<int16_t> ring[NANT], rot, pilot[2][4]; /* pilot[dmrs][layer]: c16 pairs of this grant, 6 per RB (type 1) */
  uint32_t ring_len = 0, ring_offset = 0;
  std::vector<int> data_syms;
  std::vector<uint8_t> tx_bits; /* transmitted bits in the LLR order */
  /* filled by run_stages */
  std::vector<int16_t> rxF_head;      /* rxdataF of antenna 0, the grant's DM-RS symbol */
  std::vector<int> chest_out;         /* delay[3] and nvar per row */
  std::vector<uint8_t> snap;          /* LDPC snapshot bytes of block 0 */
  int ldpc_rc = 0, ldpc_nsnap = 0;

  static GpuPassFixture small_grant(int nrb, int nl, int qm, int seed)
  {
    GpuPassFixture f;
    f.nrb = nrb; f.nl = nl; f.qm = qm;
    std::mt19937 rng(seed);
    std::normal_distribution<double> G(0, 1);
    std::uniform_real_distribution<double> U(0, 1);
    for (int l = 1; l < NSYM; l++)
      if (l != f.dm_sym[0] && l != f.dm_sym[1]) f.data_syms.push_back(l);
    /* channel: flat gain + one delayed tap per (ant, layer) */
    cd g0[NANT][4], g1[NANT][4];
    int dly[NANT][4];
    for (int a = 0; a < NANT; a++)
      for (int l = 0; l < nl; l++) {
        g0[a][l] = cd(G(rng), G(rng)) * (a == l ? 1.0 : 0.3);
        g1[a][l] = cd(G(rng), G(rng)) * 0.1;
        dly[a][l] = 1 + (int)(U(rng) * 3);
      }
    auto H = [&](int a, int l, int ks) { return g0[a][l] + g1[a][l] * std::polar(1.0, -2 * M_PI * ks * dly[a][l] / N); };
    std::vector<cd> Y[NANT];
    for (auto &y : Y) y.assign((size_t)NSYM * N, 0);
    for (int d = 0; d < 2; d++) {
      for (int l = 0; l < nl; l++) f.pilot[d][l].assign((size_t)2 * 6 * nrb, 0);
      for (int rb = 0; rb < nrb; rb++)
        for (int n = 0; n < 3; n++)
          for (int kp = 0; kp < 2; kp++) {
            const int bi = (int)(rng() & 1), bq = (int)(rng() & 1); /* one sequence for every port of the CDM group, as 38.211 7.4.1.1 */
            const cd r((1 - 2 * bi) / std::sqrt(2.0), (1 - 2 * bq) / std::sqrt(2.0));
            for (int l = 0; l < nl; l++) {
              const int delta = (l >> 1) & 1, wf = (l & 1) && kp ? -1 : 1;
              const int k = 12 * (f.start_rb + rb) + 4 * n + 2 * kp + delta;
              const cd p = std::conj(r) * (double)wf * 23170.0; /* chest LS multiplies rx by this and >> 15 */
              f.pilot[d][l][2 * (6 * rb + 2 * n + kp)] = (int16_t)std::lround(p.real());
              f.pilot[d][l][2 * (6 * rb + 2 * n + kp) + 1] = (int16_t)std::lround(p.imag());
              for (int a = 0; a < NANT; a++)
                Y[a][(size_t)f.dm_sym[d] * N + (FCO + k) % N] += H(a, l, k - NRB * 6) * r * (double)wf * std::sqrt(2.0);
            }
          }
    }
    for (int s : f.data_syms)
      for (int i = 0; i < 12 * nrb; i++) {
        const int k = 12 * f.start_rb + i;
        for (int l = 0; l < nl; l++) {
          int ci = 0, cq = 0;
          for (int b = 0; b < qm; b++) {
            const int bit = (int)(rng() & 1);
            f.tx_bits.push_back((uint8_t)bit);
            if (b & 1) cq |= bit << (b / 2); else ci |= bit << (b / 2);
          }
          const cd x(pam(qm, ci), pam(qm, cq));
          for (int a = 0; a < NANT; a++) Y[a][(size_t)s * N + (FCO + k) % N] += H(a, l, k - NRB * 6) * x;
        }
      }
    f.rot.resize(2 * NSYM);
    {
      const double Tc = 1 / 480e3 / 4096, Nu = 2048 * 64 / 2.0, Ncp0 = 16 * 64 + 144 * 64 / 2.0, Ncp1 = 144 * 64 / 2.0;
      double tl = 0;
      for (int l = 0; l < NSYM; l++) {
        const double poff = 2 * M_PI * (tl + (l == 0 ? Ncp0 : Ncp1) * Tc) * 3.75e9;
        f.rot[2 * l] = (int16_t)std::floor(std::cos(poff) * 32767);
        f.rot[2 * l + 1] = (int16_t)std::floor(std::sin(-poff) * 32767);
        tl += (Nu + (l == 0 ? Ncp0 : Ncp1)) * Tc;
      }
    }
    f.ring_len = 3 * SLOT;
    f.ring_offset = f.ring_len - 1000; /* the slot wraps around the ring end */
    const double noise_sd = std::pow(10, -20.0 / 20) / std::sqrt(2.0); /* 20 dB */
    for (int a = 0; a < NANT; a++) {
      for (auto &y : Y[a]) y += cd(G(rng), G(rng)) * noise_sd;
      std::vector<double> td(2 * (SLOT + 64), 0.0);
      for (int l = 0; l < NSYM; l++) {
        std::vector<cd> v(Y[a].begin() + (size_t)l * N, Y[a].begin() + (size_t)(l + 1) * N);
        for (auto &x : v) x *= 40.0;
        ifft(v);
        const cd rc(f.rot[2 * l] / 32767.0, f.rot[2 * l + 1] / 32767.0);
        const int ncp = l == 0 ? NCP0 : NCP, start = l == 0 ? 0 : NCP0 + N + (l - 1) * (N + NCP);
        for (int n = 0; n < ncp + N; n++) {
          const cd s = v[(n - ncp + N) % N] * rc;
          td[2 * (start + n)] = s.real();
          td[2 * (start + n) + 1] = s.imag();
        }
      }
      f.ring[a].assign((size_t)2 * f.ring_len, 0);
      for (int n = 0; n < SLOT + 64; n++) {
        const size_t p = (f.ring_offset + n) % f.ring_len;
        f.ring[a][2 * p] = (int16_t)std::lround(std::max(-32768.0, std::min(32767.0, td[2 * n])));
        f.ring[a][2 * p + 1] = (int16_t)std::lround(std::max(-32768.0, std::min(32767.0, td[2 * n + 1])));
      }
    }
    return f;
  }

  /* 0 = all four stages ran (LDPC may have reported -2 on CC < 9: ldpc_rc); the int16 LLRs of the grant in nr_rx_pdsch order (symbol,
   * RE, layer, bit; positive = bit 0) go to *llr. Buffers use the model nr_gpu_model() gives now. */
  int run_stages(std::vector<int16_t> *llr, bool old_path = false)
  {
    bool ok = false;
    GpuPassModule &m = GpuPassModule::get(&ok);
    if (!ok) return -1;
    const int len = 12 * nrb, ext = (len + 15) & ~15, nsym = (int)data_syms.size();
    const size_t rxsz = (size_t)NANT * NSYM * N * 4, estrow = (size_t)NSYM * N * 4;
    nr_gpu_buf_t rxF = {}, est = {}, nvar = {}, geo = {}, scratch = {}, llrb = {}, lin = {}, bits = {};
    int rc = -1;
    cudaStream_t s = nullptr;
    const int nrows = 2 * nl * NANT;
    const size_t bn = (size_t)2 * NSYM * NR_GPU_DEMOD_MAX_NL * ext * 2; /* comp, mag, magb, magr: int16 each */
    if (nr_gpu_buf_alloc(&rxF, rxsz) || nr_gpu_buf_alloc(&est, (size_t)nl * NANT * estrow) || nr_gpu_buf_alloc(&nvar, NR_GPU_CHEST_DEV_BYTES)
        || nr_gpu_buf_alloc(&geo, (size_t)2 * 2 * NSYM * ext * 2) || nr_gpu_buf_alloc(&scratch, 4 * bn * 2)
        || nr_gpu_buf_alloc(&llrb, (size_t)nsym * len * qm * nl * 2) || nr_gpu_buf_alloc(&lin, NR_GPU_LDPC_DEV_IN_BYTES)
        || nr_gpu_buf_alloc(&bits, NR_GPU_LDPC_DEV_BITS_BYTES) || cudaStreamCreate(&s))
      goto out;
    {
      std::vector<const int16_t *> rp(NANT);
      for (int a = 0; a < NANT; a++) rp[a] = ring[a].data();
      nr_gpu_fep_job_t fj = {rp.data(), ring_len, ring_offset, rot.data(), 0x3fff};
      if (old_path ? m.fep_old(rp.data(), ring_len, ring_offset, 0.0, rot.data(), 0x3fff, (int16_t *)rxF.host, (size_t)NSYM * N)
                   : m.fep(&fj, &rxF, s))
        goto out;
      /* chest: rows (DM-RS symbol, layer, antenna); pilots live in a device-visible buffer too */
      std::vector<nr_gpu_chest_job_t> keep(1);
      nr_gpu_chest_job_t &cj = keep[0];
      memset(&cj, 0, sizeof(cj));
      nr_gpu_buf_t pil = {};
      if (nr_gpu_buf_alloc(&pil, (size_t)2 * nl * 12 * nrb * 2 * 2)) goto out;
      cj.type = 1; cj.N = N; cj.n_rows = nrows;
      /* the OAI delay table (init_delay_table): 256 * exp(+j 2 pi k delay / N), delay -20..20 */
      nr_gpu_buf_t dt = {};
      const int MDC = 20;
      if (nr_gpu_buf_alloc(&dt, (size_t)(2 * MDC + 1) * N * 4)) { nr_gpu_buf_free(&pil); goto out; }
      cj.max_delay_comp = MDC; cj.dt_stride = N;
      for (int d = -MDC; d <= MDC; d++)
        for (int i = 0; i < N; i++) {
          const double ph = 2.0 * M_PI * i * d / N;
          ((int16_t *)dt.host)[2 * ((size_t)(MDC + d) * N + i)] = (int16_t)std::lround(256 * std::cos(ph));
          ((int16_t *)dt.host)[2 * ((size_t)(MDC + d) * N + i) + 1] = (int16_t)std::lround(256 * std::sin(ph));
        }
      nr_gpu_buf_h2d_async(&dt, 0, dt.bytes, s);
      cj.delay_table = (const int16_t *)dt.dev;
      memcpy(cj.filt[0], filt16_ul_p0, 32);
      memcpy(cj.filt[1], filt16_ul_p1p2, 32);
      memcpy(cj.filt[2], filt16_ul_middle, 32);
      memcpy(cj.filt[3], filt16_ul_last, 32);
      int r = 0;
      for (int d = 0; d < 2; d++)
        for (int l = 0; l < nl; l++) {
          int16_t *ph = (int16_t *)pil.host + (size_t)(d * nl + l) * 12 * nrb * 2;
          memcpy(ph, pilot[d][l].data(), pilot[d][l].size() * 2);
          for (int a = 0; a < NANT; a++, r++) {
            nr_gpu_chest_row_t &row = cj.rows[r];
            row.rxF = (const int16_t *)rxF.dev + 2 * ((size_t)a * NSYM * N + (size_t)dm_sym[d] * N + (l >> 1 & 1));
            row.pil = (const int16_t *)pil.dev + (size_t)(d * nl + l) * 12 * nrb * 2;
            row.out = (int16_t *)est.dev + 2 * ((size_t)(l * NANT + a) * NSYM * N + (size_t)dm_sym[d] * N);
            row.k0 = (FCO + 12 * start_rb) % N;
            row.nb_rb = nrb;
          }
        }
      nr_gpu_buf_h2d_async(&pil, 0, pil.bytes, s);
      rc = old_path ? m.chest_old(&cj) : m.chest(&cj, nrows, &est, &nvar, s);
      if (rc) { nr_gpu_buf_free(&pil); nr_gpu_buf_free(&dt); rc = -1; goto out; }
      nr_gpu_buf_d2h_async(&nvar, 0, 4 * (size_t)NR_GPU_CHEST_MAX_ROWS * 4, s);
      nr_gpu_buf_d2h_async(&rxF, 0, rxsz, s);
      if (cudaStreamSynchronize(s)) { nr_gpu_buf_free(&pil); nr_gpu_buf_free(&dt); rc = -1; goto out; }
      nr_gpu_buf_free(&pil);
      nr_gpu_buf_free(&dt);
      std::vector<int> old_cout;
      if (old_path) { /* the old entry returns delay[][3] and nvar[] in the job */
        old_cout.assign(4 * NR_GPU_CHEST_MAX_ROWS, 0);
        memcpy(old_cout.data(), cj.delay, 3 * nrows * sizeof(int));
        memcpy(old_cout.data() + 3 * NR_GPU_CHEST_MAX_ROWS, cj.nvar, nrows * sizeof(int));
      }
      const int *cout = old_path ? old_cout.data() : (const int *)nvar.host;
      chest_out.assign(cout, cout + 3 * nrows); /* delay triples, then nvar per row, then the mean */
      uint32_t nv = 0;
      for (int q = 0; q < nrows; q++) {
        chest_out.push_back(cout[3 * NR_GPU_CHEST_MAX_ROWS + q]);
        nv += (unsigned)cout[3 * NR_GPU_CHEST_MAX_ROWS + q];
      }
      chest_out.push_back((int)(nv / nrows));
      if (getenv("GP_DBG")) {
        const int16_t *x = (const int16_t *)rxF.host;
        const int k = (FCO + 12 * start_rb) % N;
        for (int sy : {2, 3, 5}) printf("rxF a0 sym%d k=%d..: %d,%d %d,%d %d,%d\n", sy, k, x[2 * (sy * N + k)], x[2 * (sy * N + k) + 1], x[2 * (sy * N + k + 1)], x[2 * (sy * N + k + 1) + 1], x[2 * (sy * N + k + 2)], x[2 * (sy * N + k + 2) + 1]);
        nr_gpu_buf_d2h_async(&est, 0, est.bytes, s);
        cudaStreamSynchronize(s);
        const int16_t *e = (const int16_t *)est.host;
        for (int row = 0; row < nl * NANT; row++) printf("est row %d sym2 k=%d: %d,%d %d,%d  nvar %d\n", row, k, e[2 * ((size_t)row * NSYM * N + 2 * N)], e[2 * ((size_t)row * NSYM * N + 2 * N) + 1], e[2 * ((size_t)row * NSYM * N + 2 * N + 5)], e[2 * ((size_t)row * NSYM * N + 2 * N + 5) + 1], chest_out[3 * nrows + row]);
      }
      rxF_head.assign((int16_t *)rxF.host, (int16_t *)rxF.host + 2 * (size_t)NSYM * N);
      /* demod: hold the nearest DM-RS estimate; geometry = the grant's contiguous REs */
      int16_t *gy = (int16_t *)geo.host, *gh = gy + 2 * NSYM * ext;
      for (int sy : data_syms)
        for (int i = 0; i < len; i++) {
          const int k = (FCO + 12 * start_rb + i) % N; /* rxdataF index; the estimate row is indexed from the grant's first RE */
          gy[2 * (sy * ext + i)] = (int16_t)k;
          gy[2 * (sy * ext + i) + 1] = 1;
          gh[2 * (sy * ext + i)] = (int16_t)i;
          gh[2 * (sy * ext + i) + 1] = (int16_t)((sy < 7 ? dm_sym[0] : dm_sym[1]) + 1);
        }
      nr_gpu_buf_h2d_async(&geo, 0, geo.bytes, s);
      nr_gpu_demod_job_t dj = {};
      dj.nbRx = NANT; dj.nl = nl; dj.qm = qm; dj.N = N;
      dj.start_symbol = data_syms.front(); dj.nb_symbols = 13; dj.first_symbol = data_syms.front();
      dj.rx_stride = NSYM * N; dj.est_stride = NSYM * N; dj.buf = ext; dj.ext = ext;
      uint32_t off = 0;
      for (int sy : data_syms) {
        dj.sym_mask |= (uint16_t)(1u << sy);
        dj.nre[sy] = (uint16_t)len;
        dj.llr_off[sy] = off;
        off += len * qm * nl;
      }
      dj.rxdataF = (const int16_t *)rxF.dev; dj.est = (const int16_t *)est.dev;
      dj.geo_y = (const int16_t *)geo.dev; dj.geo_h = (const int16_t *)geo.dev + 2 * NSYM * ext;
      int16_t *sc = (int16_t *)scratch.dev;
      dj.comp = sc; dj.mag = sc + bn; dj.magb = sc + 2 * bn; dj.magr = sc + 3 * bn;
      dj.thr_coef = std::pow(10.0, -1.2);
      dj.qam_mode = nl >= 2 ? 3 : 0; dj.mag_per_layer = 1; dj.nvar = (uint32_t)chest_out.back(); dj.m3_nvfac = std::pow(10.0, 2.1);
      if (old_path) {
        dj.llr = (int16_t *)llrb.host;
        if (m.demod_old(&dj)) goto out;
        m.demod_rel(dj.ctx);
      } else if (m.demod(&dj, &llrb, s))
        goto out;
      nr_gpu_buf_d2h_async(&llrb, 0, llrb.bytes, s);
      if (cudaStreamSynchronize(s)) goto out;
      llr->assign((int16_t *)llrb.host, (int16_t *)llrb.host + (size_t)nsym * len * qm * nl);
      /* LDPC on the first BG2 code block of the LLR stream (hard-limited to int8): bit parity only */
      nr_gpu_ldpc_job_t lj = {};
      lj.BG = 2; lj.R = 15; lj.Z = 64; lj.K = 640; lj.it_max = 8;
      int8_t *in = (int8_t *)lin.host;
      for (size_t i = 0; i < (size_t)52 * 64; i++) in[i] = (int8_t)std::max(-127, std::min(127, (int)(*llr)[i % llr->size()] / 16));
      nr_gpu_buf_h2d_async(&lin, 0, (size_t)52 * 64, s);
      if (old_path) {
        lj.llr = in;
        std::vector<nr_gpu_ldpc_job_t> hold(1, lj); /* the job holds 17 KB of snapshots */
        ldpc_rc = m.ldpc_old(hold.data(), 1);
        if (ldpc_rc == 0) {
          ldpc_nsnap = hold[0].nsnap;
          snap.clear();
          for (int q = 0; q < ldpc_nsnap; q++) snap.insert(snap.end(), hold[0].snap[q], hold[0].snap[q] + (lj.K >> 3));
          snap.push_back((uint8_t)hold[0].syn0);
        }
      } else {
      ldpc_rc = m.ldpc(&lj, 1, &lin, &bits, s);
      if (ldpc_rc == 0) {
        nr_gpu_buf_d2h_async(&bits, 0, bits.bytes, s);
        if (cudaStreamSynchronize(s)) goto out;
        const int *res = (const int *)((char *)bits.host + NR_GPU_LDPC_DEV_RES_OFF);
        ldpc_nsnap = res[0];
        snap.clear(); /* only the K/8 bytes of each written snapshot are defined, as the old entry copies them */
        for (int q = 0; q < ldpc_nsnap; q++)
          snap.insert(snap.end(), (uint8_t *)bits.host + (size_t)q * NR_GPU_LDPC_KB_MAX, (uint8_t *)bits.host + (size_t)q * NR_GPU_LDPC_KB_MAX + (lj.K >> 3));
        snap.push_back((uint8_t)res[1]);
      }
      }
      rc = 0;
    }
  out:
    if (s) cudaStreamDestroy(s);
    nr_gpu_buf_free(&rxF); nr_gpu_buf_free(&est); nr_gpu_buf_free(&nvar); nr_gpu_buf_free(&geo); nr_gpu_buf_free(&scratch);
    nr_gpu_buf_free(&llrb); nr_gpu_buf_free(&lin); nr_gpu_buf_free(&bits);
    return rc;
  }

 private:
  static double pam(int qm, int code)
  {
    const int h = qm / 2;
    const int b[4] = {code & 1, (code >> 1) & 1, (code >> 2) & 1, (code >> 3) & 1};
    double a;
    if (h == 1) a = 1;
    else if (h == 2) a = 2 - (1 - 2 * b[1]);
    else if (h == 3) a = 4 - (1 - 2 * b[1]) * (2 - (1 - 2 * b[2]));
    else a = 8 - (1 - 2 * b[1]) * (4 - (1 - 2 * b[2]) * (2 - (1 - 2 * b[3])));
    a *= 1 - 2 * b[0];
    return a / std::sqrt(qm == 2 ? 2 : qm == 4 ? 10 : qm == 6 ? 42 : 170);
  }
  static void ifft(std::vector<cd> &v)
  {
    int n = v.size();
    for (int i = 1, jj = 0; i < n; i++) {
      int bit = n >> 1;
      for (; jj & bit; bit >>= 1) jj ^= bit;
      jj ^= bit;
      if (i < jj) std::swap(v[i], v[jj]);
    }
    for (int len = 2; len <= n; len <<= 1) {
      const cd w = std::polar(1.0, 2 * M_PI / len);
      for (int i = 0; i < n; i += len) {
        cd wk = 1;
        for (int q = 0; q < len / 2; q++) {
          const cd u = v[i + q], t = v[i + q + len / 2] * wk;
          v[i + q] = u + t;
          v[i + q + len / 2] = u - t;
          wk *= w;
        }
      }
    }
  }
};
#endif
