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
  nr_gpu_fep_stage_fn fep_stage = nullptr;
  nr_gpu_chest_stage_fn chest_stage = nullptr;
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
        m.fep_stage = (nr_gpu_fep_stage_fn)dlsym(m.dl, "nr_pdsch_gpu_fep_stage");
        m.chest_stage = (nr_gpu_chest_stage_fn)dlsym(m.dl, "nr_pdsch_gpu_chest_stage");
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
        good = api && m.fep_stage && m.chest_stage && m.fep_old && m.chest_old && m.demod_old && m.demod_rel && m.ldpc_old && dinit && linit && m.fep && m.chest && m.demod && m.ldpc && api()->init(&cfg) == 0 && dinit(4) == 0;
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

  /* Everything of one grant shape that a captured graph bakes in: fixed device buffers, a fixed host IQ ring and the four jobs. load() feeds
   * a new input into the same addresses. nvar_fixed >= 0 pins the demod's by-value nvar (a graph cannot take it from the chest). */
  struct Out {
    std::vector<int16_t> rxF, llr;
    std::vector<int> chest; /* delay triples, then nvar per row */
    std::vector<uint8_t> snap;
    int ldpc_rc = 0, ldpc_nsnap = 0;
    bool operator==(const Out &o) const { return rxF == o.rxF && llr == o.llr && chest == o.chest && snap == o.snap && ldpc_rc == o.ldpc_rc && ldpc_nsnap == o.ldpc_nsnap; }
  };
  struct Pass {
    GpuPassModule *m = nullptr;
    const GpuPassFixture *fx = nullptr;
    nr_gpu_buf_t rxF = {}, est = {}, nvar = {}, geo = {}, scratch = {}, llrb = {}, lin = {}, bits = {}, pil = {}, dt = {};
    std::vector<nr_gpu_chest_job_t> cjv = std::vector<nr_gpu_chest_job_t>(1);
    nr_gpu_demod_job_t dj = {};
    nr_gpu_ldpc_job_t lj = {};
    std::vector<int16_t> pring[NANT], prot;
    std::vector<const int16_t *> rp;
    nr_gpu_fep_job_t fj = {};
    cudaStream_t s = nullptr;
    int nrows = 0, len = 0, ext = 0, nsym = 0, nvar_fixed = -1;
    size_t rxsz = 0, bn = 0;
    Pass() = default;
    Pass(const Pass &) = delete;
    ~Pass()
    {
      for (nr_gpu_buf_t *b : {&rxF, &est, &nvar, &geo, &scratch, &llrb, &lin, &bits, &pil, &dt}) nr_gpu_buf_free(b);
      if (s) cudaStreamDestroy(s);
    }
    /* buffers (current nr_gpu_model()), jobs, fixed ring; false = failure */
    bool setup(GpuPassModule &mod, const GpuPassFixture &f, int nvar_pin)
    {
      m = &mod; fx = &f; nvar_fixed = nvar_pin;
      len = 12 * f.nrb; ext = (len + 15) & ~15; nsym = (int)f.data_syms.size(); nrows = 2 * f.nl * NANT;
      rxsz = (size_t)NANT * NSYM * N * 4;
      const size_t estrow = (size_t)NSYM * N * 4;
      bn = (size_t)2 * NSYM * NR_GPU_DEMOD_MAX_NL * ext * 2; /* comp, mag, magb, magr: int16 each */
      if (nr_gpu_buf_alloc(&rxF, rxsz) || nr_gpu_buf_alloc(&est, (size_t)f.nl * NANT * estrow) || nr_gpu_buf_alloc(&nvar, NR_GPU_CHEST_DEV_BYTES)
          || nr_gpu_buf_alloc(&geo, (size_t)2 * 2 * NSYM * ext * 2) || nr_gpu_buf_alloc(&scratch, 4 * bn * 2)
          || nr_gpu_buf_alloc(&llrb, (size_t)nsym * len * f.qm * f.nl * 2) || nr_gpu_buf_alloc(&lin, NR_GPU_LDPC_DEV_IN_BYTES)
          || nr_gpu_buf_alloc(&bits, NR_GPU_LDPC_DEV_BITS_BYTES) || nr_gpu_buf_alloc(&pil, (size_t)2 * f.nl * 12 * f.nrb * 2 * 2)
          || nr_gpu_buf_alloc(&dt, (size_t)(2 * 20 + 1) * N * 4) || cudaStreamCreate(&s))
        return false;
      for (int a = 0; a < NANT; a++) pring[a].assign((size_t)2 * f.ring_len, 0);
      prot.assign(2 * NSYM, 0);
      rp.resize(NANT);
      for (int a = 0; a < NANT; a++) rp[a] = pring[a].data();
      fj = {rp.data(), f.ring_len, f.ring_offset, prot.data(), 0x3fff};
      /* the OAI delay table (init_delay_table): 256 * exp(+j 2 pi k delay / N), delay -20..20 */
      const int MDC = 20;
      for (int d = -MDC; d <= MDC; d++)
        for (int i = 0; i < N; i++) {
          const double ph = 2.0 * M_PI * i * d / N;
          ((int16_t *)dt.host)[2 * ((size_t)(MDC + d) * N + i)] = (int16_t)std::lround(256 * std::cos(ph));
          ((int16_t *)dt.host)[2 * ((size_t)(MDC + d) * N + i) + 1] = (int16_t)std::lround(256 * std::sin(ph));
        }
      nr_gpu_buf_h2d_async(&dt, 0, dt.bytes, s);
      nr_gpu_chest_job_t &cj = cjv[0];
      memset(&cj, 0, sizeof(cj));
      cj.type = 1; cj.N = N; cj.n_rows = nrows; cj.max_delay_comp = MDC; cj.dt_stride = N;
      cj.delay_table = (const int16_t *)dt.dev;
      memcpy(cj.filt[0], filt16_ul_p0, 32);
      memcpy(cj.filt[1], filt16_ul_p1p2, 32);
      memcpy(cj.filt[2], filt16_ul_middle, 32);
      memcpy(cj.filt[3], filt16_ul_last, 32);
      int r = 0;
      for (int d = 0; d < 2; d++)
        for (int l = 0; l < f.nl; l++)
          for (int a = 0; a < NANT; a++, r++) {
            nr_gpu_chest_row_t &row = cj.rows[r];
            row.rxF = (const int16_t *)rxF.dev + 2 * ((size_t)a * NSYM * N + (size_t)f.dm_sym[d] * N + (l >> 1 & 1));
            row.pil = (const int16_t *)pil.dev + (size_t)(d * f.nl + l) * 12 * f.nrb * 2;
            row.out = (int16_t *)est.dev + 2 * ((size_t)(l * NANT + a) * NSYM * N + (size_t)f.dm_sym[d] * N);
            row.k0 = (FCO + 12 * f.start_rb) % N;
            row.nb_rb = f.nrb;
          }
      /* demod: hold the nearest DM-RS estimate; geometry = the grant's contiguous REs */
      int16_t *gy = (int16_t *)geo.host, *gh = gy + 2 * NSYM * ext;
      for (int sy : f.data_syms)
        for (int i = 0; i < len; i++) {
          gy[2 * (sy * ext + i)] = (int16_t)((FCO + 12 * f.start_rb + i) % N); /* rxdataF index; estimates are indexed from the grant's first RE */
          gy[2 * (sy * ext + i) + 1] = 1;
          gh[2 * (sy * ext + i)] = (int16_t)i;
          gh[2 * (sy * ext + i) + 1] = (int16_t)((sy < 7 ? f.dm_sym[0] : f.dm_sym[1]) + 1);
        }
      nr_gpu_buf_h2d_async(&geo, 0, geo.bytes, s);
      dj.nbRx = NANT; dj.nl = f.nl; dj.qm = f.qm; dj.N = N;
      dj.start_symbol = f.data_syms.front(); dj.nb_symbols = 13; dj.first_symbol = f.data_syms.front();
      dj.rx_stride = NSYM * N; dj.est_stride = NSYM * N; dj.buf = ext; dj.ext = ext;
      uint32_t off = 0;
      for (int sy : f.data_syms) {
        dj.sym_mask |= (uint16_t)(1u << sy);
        dj.nre[sy] = (uint16_t)len;
        dj.llr_off[sy] = off;
        off += len * f.qm * f.nl;
      }
      dj.rxdataF = (const int16_t *)rxF.dev; dj.est = (const int16_t *)est.dev;
      dj.geo_y = (const int16_t *)geo.dev; dj.geo_h = (const int16_t *)geo.dev + 2 * NSYM * ext;
      int16_t *sc = (int16_t *)scratch.dev;
      dj.comp = sc; dj.mag = sc + bn; dj.magb = sc + 2 * bn; dj.magr = sc + 3 * bn;
      dj.thr_coef = std::pow(10.0, -1.2);
      dj.qam_mode = f.nl >= 2 ? 3 : 0; dj.mag_per_layer = 1; dj.m3_nvfac = std::pow(10.0, 2.1);
      if (nvar_fixed >= 0) dj.nvar = (uint32_t)nvar_fixed;
      lj.BG = 2; lj.R = 15; lj.Z = 64; lj.K = 640; lj.it_max = 8;
      if (nvar_fixed >= 0) { /* graph mode: the LDPC input cannot come from the demod (no device int16 -> int8 stage), so it is a fixed pattern */
        uint32_t x = 12345;
        for (size_t i = 0; i < (size_t)52 * 64; i++) { x = x * 1664525u + 1013904223u; ((int8_t *)lin.host)[i] = (int8_t)((int)(x >> 24) % 81 - 40); }
        nr_gpu_buf_h2d_async(&lin, 0, (size_t)52 * 64, s);
      }
      return cudaStreamSynchronize(s) == cudaSuccess;
    }
    /* feeds the fixture f (same shape) into the fixed ring / rotation / pilots; the output buffers are cleared so a stage that does not run shows */
    bool load(const GpuPassFixture &f)
    {
      for (int a = 0; a < NANT; a++) memcpy(pring[a].data(), f.ring[a].data(), pring[a].size() * 2);
      memcpy(prot.data(), f.rot.data(), prot.size() * 2);
      fj.ring_offset = f.ring_offset;
      for (int d = 0; d < 2; d++)
        for (int l = 0; l < f.nl; l++)
          memcpy((int16_t *)pil.host + (size_t)(d * f.nl + l) * 12 * f.nrb * 2, f.pilot[d][l].data(), f.pilot[d][l].size() * 2);
      nr_gpu_buf_h2d_async(&pil, 0, pil.bytes, s);
      return cudaStreamSynchronize(s) == cudaSuccess && !cudaMemset(rxF.dev, 0, rxsz) && !cudaMemset(est.dev, 0, est.bytes)
             && !cudaMemset(nvar.dev, 0, nvar.bytes) && !cudaMemset(llrb.dev, 0, llrb.bytes) && !cudaMemset(bits.dev, 0, bits.bytes);
    }
    int stage(int *fs, int *cs) { return m->fep_stage(&fj, fs) || m->chest_stage(&cjv[0], nrows, cs); }
    /* the launch-only part, exactly what a graph captures (LDPC rc is returned through *ldpc_rc: -2 below CC 9 is not an error) */
    int enqueue_dev(int fs, int cs, cudaStream_t st, int *ldpc_rc)
    {
      const int e1 = m->fep(&fj, &rxF, fs, st), e2 = m->chest(&cjv[0], nrows, &est, &nvar, cs, st), e3 = m->demod(&dj, &llrb, st);
      if (e1 || e2 || e3) { fprintf(stderr, "enqueue_dev: fep %d chest %d demod %d (slots %d %d) last=%s\n", e1, e2, e3, fs, cs, cudaGetErrorString(cudaPeekAtLastError())); return -1; }
      *ldpc_rc = m->ldpc(&lj, 1, &lin, &bits, st);
      return *ldpc_rc == 0 || *ldpc_rc == -2 ? 0 : -1;
    }
    /* D2H of every output (on s) and a snapshot of it */
    bool fetch(Out *o, int ldpc_rc)
    {
      nr_gpu_buf_d2h_async(&rxF, 0, rxsz, s);
      nr_gpu_buf_d2h_async(&nvar, 0, nvar.bytes, s);
      nr_gpu_buf_d2h_async(&llrb, 0, llrb.bytes, s);
      if (ldpc_rc == 0) nr_gpu_buf_d2h_async(&bits, 0, bits.bytes, s);
      if (cudaStreamSynchronize(s)) return false;
      o->rxF.assign((int16_t *)rxF.host, (int16_t *)rxF.host + 2 * (size_t)NANT * NSYM * N);
      o->llr.assign((int16_t *)llrb.host, (int16_t *)llrb.host + (size_t)nsym * len * fx->qm * fx->nl);
      const int *c = (const int *)nvar.host;
      o->chest.assign(c, c + 3 * nrows);
      for (int q = 0; q < nrows; q++) o->chest.push_back(c[3 * NR_GPU_CHEST_MAX_ROWS + q]);
      o->ldpc_rc = ldpc_rc; o->ldpc_nsnap = 0; o->snap.clear();
      if (ldpc_rc == 0) {
        const int *res = (const int *)((char *)bits.host + NR_GPU_LDPC_DEV_RES_OFF);
        o->ldpc_nsnap = res[0];
        for (int q = 0; q < res[0]; q++)
          o->snap.insert(o->snap.end(), (uint8_t *)bits.host + (size_t)q * NR_GPU_LDPC_KB_MAX, (uint8_t *)bits.host + (size_t)q * NR_GPU_LDPC_KB_MAX + (lj.K >> 3));
        o->snap.push_back((uint8_t)res[1]);
      }
      return true;
    }
  };

  /* 0 = all four stages ran (LDPC may have reported -2 on CC < 9: ldpc_rc); the int16 LLRs of the grant in nr_rx_pdsch order (symbol,
   * RE, layer, bit; positive = bit 0) go to *llr. Buffers use the model nr_gpu_model() gives now. old_path: the pre-GPUPASS entry points
   * with their own waits (DIRECT only). The nvar of the demod job is the chest mean (host sync between the stages), the LDPC input is the
   * LLR stream hard-limited to int8. */
  int run_stages(std::vector<int16_t> *llr, bool old_path = false)
  {
    bool ok = false;
    GpuPassModule &m = GpuPassModule::get(&ok);
    if (!ok) return -1;
    Pass P;
    if (!P.setup(m, *this, -1) || !P.load(*this)) return -1;
    cudaStream_t s = P.s;
    nr_gpu_chest_job_t &cj = P.cjv[0];
    int fslot = 0, cslot = 0, rc;
    if (old_path) {
      if (m.fep_old(P.rp.data(), ring_len, ring_offset, 0.0, rot.data(), 0x3fff, (int16_t *)P.rxF.host, (size_t)NSYM * N)) return -1;
      rc = m.chest_old(&cj);
    } else {
      if (P.stage(&fslot, &cslot) || m.fep(&P.fj, &P.rxF, fslot, s)) return -1;
      rc = m.chest(&cj, P.nrows, &P.est, &P.nvar, cslot, s);
    }
    if (rc) return -1;
    nr_gpu_buf_d2h_async(&P.nvar, 0, P.nvar.bytes, s);
    nr_gpu_buf_d2h_async(&P.rxF, 0, P.rxsz, s);
    if (cudaStreamSynchronize(s)) return -1;
    const int nrows = P.nrows;
    std::vector<int> cout_v(4 * NR_GPU_CHEST_MAX_ROWS, 0);
    if (old_path) { /* the old entry returns delay[][3] and nvar[] in the job */
      memcpy(cout_v.data(), cj.delay, 3 * nrows * sizeof(int));
      memcpy(cout_v.data() + 3 * NR_GPU_CHEST_MAX_ROWS, cj.nvar, nrows * sizeof(int));
    } else
      memcpy(cout_v.data(), P.nvar.host, cout_v.size() * sizeof(int));
    const int *cout = cout_v.data();
    chest_out.assign(cout, cout + 3 * nrows); /* delay triples, then nvar per row, then the mean */
    uint32_t nv = 0;
    for (int q = 0; q < nrows; q++) {
      chest_out.push_back(cout[3 * NR_GPU_CHEST_MAX_ROWS + q]);
      nv += (unsigned)cout[3 * NR_GPU_CHEST_MAX_ROWS + q];
    }
    chest_out.push_back((int)(nv / nrows));
    rxF_head.assign((int16_t *)P.rxF.host, (int16_t *)P.rxF.host + 2 * (size_t)NSYM * N);
    nr_gpu_demod_job_t &dj = P.dj;
    dj.nvar = (uint32_t)chest_out.back();
    if (old_path) {
      dj.llr = (int16_t *)P.llrb.host;
      if (m.demod_old(&dj)) return -1;
      m.demod_rel(dj.ctx);
    } else if (m.demod(&dj, &P.llrb, s))
      return -1;
    nr_gpu_buf_d2h_async(&P.llrb, 0, P.llrb.bytes, s);
    if (cudaStreamSynchronize(s)) return -1;
    llr->assign((int16_t *)P.llrb.host, (int16_t *)P.llrb.host + (size_t)P.nsym * P.len * qm * nl);
    /* LDPC on the first BG2 code block of the LLR stream (hard-limited to int8): bit parity only */
    nr_gpu_ldpc_job_t &lj = P.lj;
    int8_t *in = (int8_t *)P.lin.host;
    for (size_t i = 0; i < (size_t)52 * 64; i++) in[i] = (int8_t)std::max(-127, std::min(127, (int)(*llr)[i % llr->size()] / 16));
    nr_gpu_buf_h2d_async(&P.lin, 0, (size_t)52 * 64, s);
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
      ldpc_rc = m.ldpc(&lj, 1, &P.lin, &P.bits, s);
      if (ldpc_rc == 0) {
        nr_gpu_buf_d2h_async(&P.bits, 0, P.bits.bytes, s);
        if (cudaStreamSynchronize(s)) return -1;
        const int *res = (const int *)((char *)P.bits.host + NR_GPU_LDPC_DEV_RES_OFF);
        ldpc_nsnap = res[0];
        snap.clear(); /* only the K/8 bytes of each written snapshot are defined, as the old entry copies them */
        for (int q = 0; q < ldpc_nsnap; q++)
          snap.insert(snap.end(), (uint8_t *)P.bits.host + (size_t)q * NR_GPU_LDPC_KB_MAX, (uint8_t *)P.bits.host + (size_t)q * NR_GPU_LDPC_KB_MAX + (lj.K >> 3));
        snap.push_back((uint8_t)res[1]);
      }
    }
    return 0;
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
