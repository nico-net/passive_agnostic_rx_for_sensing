/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* GPUPASS test fixture: one synthetic 4-RX, N = 4096 slot (own DM-RS and QAM, OAI TX symbol rotation, frequency-selective 4 x Nl
 * channel, AWGN, int16 IQ ring that wraps) pushed through the launch-only _dev entry points of libpdsch_gpu.so on ONE stream:
 * exact FEP -> exact chest -> demod -> (LDPC on CC >= 9). run_stages() allocates its device buffers under the current
 * nr_gpu_mem_test_force_discrete, so the same fixture runs both memory models. Later tasks extend it (from_shape(...)). */
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
#include <memory>
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
#include "nr_gpu_pass.h"
#include "nr_llr_norm.h"
extern "C" {
#include "common/utils/LOG/log.h"
#include "PHY/CODING/coding_defs.h"
#include "PHY/CODING/nrLDPC_coding/nrLDPC_coding_segment/nr_rate_matching.h"
}

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
  /* Task 4: the graph pass */
  nr_gpu_pass_config_fn pass_config = nullptr;
  nr_gpu_pass_init_fn pass_init = nullptr;
  nr_gpu_pass_warm_fn pass_warm = nullptr;
  nr_gpu_pass_dl_try_fn pass_try = nullptr;
  nr_gpu_pass_stats_fn pass_stats = nullptr;
  nr_gpu_pass_counters_fn pass_counters = nullptr;
  void (*pass_hold)(unsigned, int) = nullptr;
  void (*set_spin)(int) = nullptr; /* Fix A+D bench hook: completion wait, >= 0 spin us then blocking sync, < 0 the former poll+nap */
  void (*pass_split)(int) = nullptr; /* v2 Task 3 test hook: the split (C / D / L sub-graph) mode on / off */
  void (*call_begin)(void) = nullptr, (*call_end)(void) = nullptr, (*call_dirty)(void) = nullptr; /* v2 Task 4: the held instance of a decode call */
  void (*retry_counters)(uint64_t *) = nullptr;                                                      /* entries C, D, held calls, dirty */
  int pass_state = 0; /* 0 not tried, 1 ready, -1 failed */
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
        if (auto fp32 = (void (*)(int))dlsym(m.dl, "nr_gpu_pass_test_fp32")) fp32(0); /* the bit-exact double demod is the reference here (FP32 is the production default) */
        m.pass_config = (nr_gpu_pass_config_fn)dlsym(m.dl, "nr_gpu_pass_config");
        m.pass_init = (nr_gpu_pass_init_fn)dlsym(m.dl, "nr_gpu_pass_init");
        m.pass_warm = (nr_gpu_pass_warm_fn)dlsym(m.dl, "nr_gpu_pass_warm");
        m.pass_try = (nr_gpu_pass_dl_try_fn)dlsym(m.dl, "nr_gpu_pass_dl_try");
        m.pass_stats = (nr_gpu_pass_stats_fn)dlsym(m.dl, "nr_gpu_pass_stats");
        m.pass_counters = (nr_gpu_pass_counters_fn)dlsym(m.dl, "nr_gpu_pass_counters");
        m.pass_hold = (void (*)(unsigned, int))dlsym(m.dl, "nr_gpu_pass_test_hold");
        m.set_spin = (void (*)(int))dlsym(m.dl, "nr_gpu_pass_set_spin_us");
        m.pass_split = (void (*)(int))dlsym(m.dl, "nr_gpu_pass_test_split");
        m.call_begin = (void (*)(void))dlsym(m.dl, "nr_gpu_pass_call_begin");
        m.call_end = (void (*)(void))dlsym(m.dl, "nr_gpu_pass_call_end");
        m.call_dirty = (void (*)(void))dlsym(m.dl, "nr_gpu_pass_call_dirty");
        m.retry_counters = (void (*)(uint64_t *))dlsym(m.dl, "nr_gpu_pass_retry_counters");
        if (!m.fep) fprintf(stderr, "libpdsch_gpu.so has no nr_pdsch_gpu_fep_dev\n");
        good = api && m.fep_stage && m.chest_stage && m.fep_old && m.chest_old && m.demod_old && m.demod_rel && m.ldpc_old && dinit && linit && m.fep && m.chest && m.demod && m.ldpc && api() && api()->init(&cfg) == 0 && dinit(4) == 0;
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
  static inline int max_cb = 8;         /* from_shape(): most code blocks of a built TB (the encoder takes 8 per macro group; a test of many blocks raises it) */
  static inline int force_A = 0;        /* from_shape(): the transport block size in bits (0 = the target code rate's); for small TBs (odd lifting sizes) */
  static inline double noise_db = 20.0; /* SNR of small_grant()/from_shape() fixtures built from now on */
  static inline int widen_rb = 0;       /* v2 Task 2: contiguous fixtures built from now on estimate the channel over widen_rb more PRBs above the grant (the slot share's
                                         * widened band; the DM-RS is transmitted there too, the data is not) */
  int chest_extra = 0;
  static inline int first_sym = 1;      /* first symbol of the grant (1: the DL tests; 0: a PUSCH that starts at the slot boundary, Task 7) */
  static inline int n_dm = 2;           /* DM-RS symbols of fixtures built from now on: 2 (symbols 2, 11) or 1 (symbol 2; review S6) */
  int ndm = 2;
  static inline int dmrs_type = 1;      /* DM-RS configuration type of fixtures built from now on: 1, or 2 (v2 DMRS2: 2 pairs per RB and CDM group, layers 2-3 in group 1) */
  int dtype = 1;
  int ppr() const { return dtype == 2 ? 4 : 6; }                      /* pilots (c16) per RB */
  int delta(int l) const { return dtype == 2 ? 2 * (l >> 1 & 1) : (l >> 1 & 1); } /* get_delta of port 1000 + l */
  static constexpr int N = 4096, NCP = 288, NCP0 = 352, NSYM = 14, NRB = 273, FCO = N - NRB * 6, NANT = 4;
  static constexpr int SLOT = NCP0 + N + (NSYM - 1) * (N + NCP);
  int nrb, nl, qm, start_rb = 10;
  /* Task 4b: a segmented grant (PRG / VRB bundles). nrb is then the data PRBs (sum of n_prb); the segments sit at absolute PRBs prb_start (BWP start 0, reference point 0), their REs
   * are the data REs in segment order (data_index = first data PRB), the pilots are indexed by absolute PRB (pilspan PRBs from 0). Empty = the contiguous grant at start_rb. */
  struct SegDef { int prb_start, n_prb, data_index; };
  std::vector<SegDef> segs;
  int pilspan = 0;
  int bwp = 0, refpt = 0; /* BWPStart and the reference point of a segmented grant: segments are BWP relative, pilots are indexed by prb_start + (refpt ? 0 : bwp) */
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

  /* tx: the coded, scrambled bit stream of the grant in LLR order (symbol, RE, layer, bit); null = random bits */
  static GpuPassFixture small_grant(int nrb, int nl, int qm, int seed, const std::vector<uint8_t> *tx = nullptr, const std::vector<SegDef> *sd = nullptr, int bwp_start = 0, int ref_point = 0)
  {
    size_t txk = 0;
    GpuPassFixture f;
    f.nrb = nrb; f.nl = nl; f.qm = qm; f.ndm = n_dm; f.dtype = dmrs_type;
    f.chest_extra = sd ? 0 : widen_rb;
    f.pilspan = nrb + f.chest_extra;
    if (sd) {
      f.segs = *sd;
      f.start_rb = 0;
      f.bwp = bwp_start;
      f.refpt = ref_point;
      f.pilspan = 0;
      for (const SegDef &g : f.segs) f.pilspan = std::max(f.pilspan, g.prb_start + g.n_prb + (ref_point ? 0 : bwp_start));
    }
    auto prb_of = [&f](int rb) { /* absolute PRB of the rb-th data PRB */
      for (const SegDef &g : f.segs)
        if (rb >= g.data_index && rb < g.data_index + g.n_prb) return g.prb_start + rb - g.data_index;
      return f.start_rb + rb;
    };
    std::mt19937 rng(seed);
    std::normal_distribution<double> G(0, 1);
    std::uniform_real_distribution<double> U(0, 1);
    for (int l = first_sym; l < NSYM; l++)
      if (l != f.dm_sym[0] && (f.ndm < 2 || l != f.dm_sym[1])) f.data_syms.push_back(l);
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
    for (int d = 0; d < f.ndm; d++) {
      for (int l = 0; l < nl; l++) f.pilot[d][l].assign((size_t)2 * 6 * f.pilspan, 0);
      for (int rb = 0; rb < nrb + f.chest_extra; rb++)
        for (int n = 0; n < f.ppr() / 2; n++)
          for (int kp = 0; kp < 2; kp++) {
            const int bi = (int)(rng() & 1), bq = (int)(rng() & 1); /* one sequence for every port of the CDM group, as 38.211 7.4.1.1 */
            const cd r((1 - 2 * bi) / std::sqrt(2.0), (1 - 2 * bq) / std::sqrt(2.0));
            for (int l = 0; l < nl; l++) {
              const int delta = f.delta(l), wf = (l & 1) && kp ? -1 : 1;
              const int pr = f.bwp + prb_of(rb), pi = sd ? prb_of(rb) + (f.refpt ? 0 : f.bwp) : rb; /* absolute PRB; the pilot row index (segmented: absolute, else grant relative) */
              const int k = 12 * pr + (f.dtype == 2 ? 6 * n + kp : 4 * n + 2 * kp) + delta; /* type 2: pairs of adjacent REs 6 apart (38.211 7.4.1.1.2) */
              const cd p = std::conj(r) * (double)wf * 23170.0; /* chest LS multiplies rx by this and >> 15 */
              f.pilot[d][l][2 * (f.ppr() * pi + 2 * n + kp)] = (int16_t)std::lround(p.real());
              f.pilot[d][l][2 * (f.ppr() * pi + 2 * n + kp) + 1] = (int16_t)std::lround(p.imag());
              for (int a = 0; a < NANT; a++)
                Y[a][(size_t)f.dm_sym[d] * N + (FCO + k) % N] += H(a, l, k - NRB * 6) * r * (double)wf * std::sqrt(2.0);
            }
          }
    }
    for (int s : f.data_syms)
      for (int i = 0; i < 12 * nrb; i++) {
        const int k = 12 * (f.bwp + prb_of(i / 12)) + i % 12;
        for (int l = 0; l < nl; l++) {
          int ci = 0, cq = 0;
          for (int b = 0; b < qm; b++) {
            const int bit = tx ? (*tx)[txk++ % tx->size()] : (int)(rng() & 1);
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
    const double noise_sd = std::pow(10, -noise_db / 20) / std::sqrt(2.0); /* 20 dB unless a test sets noise_db (Task 5 SNR sweep) */
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
    nr_gpu_buf_t rxF = {}, est = {}, nvar = {}, geo = {}, scratch = {}, llrb = {}, lin = {}, bits = {}, pil = {}, dt = {}, sc = {}, vrx = {};
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
      for (nr_gpu_buf_t *b : {&rxF, &est, &nvar, &geo, &scratch, &llrb, &lin, &bits, &pil, &dt, &sc, &vrx}) nr_gpu_buf_free(b);
      if (s) cudaStreamDestroy(s);
    }
    /* buffers (current nr_gpu_model()), jobs, fixed ring; false = failure */
    bool setup(GpuPassModule &mod, const GpuPassFixture &f, int nvar_pin)
    {
      m = &mod; fx = &f; nvar_fixed = nvar_pin;
      len = 12 * f.nrb; ext = (len + 15) & ~15; nsym = (int)f.data_syms.size(); nrows = f.ndm * f.nl * NANT * (f.segs.empty() ? 1 : (int)f.segs.size());
      rxsz = (size_t)NANT * NSYM * N * 4;
      const size_t estrow = (size_t)NSYM * N * 4;
      bn = (size_t)2 * NSYM * NR_GPU_DEMOD_MAX_NL * ext * 2; /* comp, mag, magb, magr: int16 each */
      if (nr_gpu_buf_alloc(&rxF, rxsz) || nr_gpu_buf_alloc(&est, (size_t)f.nl * NANT * estrow) || nr_gpu_buf_alloc(&nvar, NR_GPU_CHEST_DEV_BYTES)
          || nr_gpu_buf_alloc(&geo, (size_t)2 * 2 * NSYM * ext * 2) || nr_gpu_buf_alloc(&scratch, 4 * bn * 2)
          || nr_gpu_buf_alloc(&llrb, (size_t)nsym * len * f.qm * f.nl * 2) || nr_gpu_buf_alloc(&lin, NR_GPU_LDPC_DEV_IN_BYTES)
          || nr_gpu_buf_alloc(&bits, NR_GPU_LDPC_DEV_BITS_BYTES) || nr_gpu_buf_alloc(&pil, (size_t)2 * f.nl * 12 * f.pilspan * 2 * 2)
          || nr_gpu_buf_alloc(&dt, (size_t)(2 * 20 + 1) * N * 4) || cudaStreamCreate(&s))
        return false;
      if (!f.segs.empty() && (nr_gpu_buf_alloc(&sc, (size_t)nrows * N * 4) || nr_gpu_buf_alloc(&vrx, rxsz))) return false;
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
      cj.type = f.dtype; cj.N = N; cj.n_rows = nrows; cj.max_delay_comp = MDC; cj.dt_stride = N;
      cj.delay_table = (const int16_t *)dt.dev;
      memcpy(cj.filt[0], filt16_ul_p0, 32);
      memcpy(cj.filt[1], filt16_ul_p1p2, 32);
      memcpy(cj.filt[2], filt16_ul_middle, 32);
      memcpy(cj.filt[3], filt16_ul_last, 32);
      int r = 0;
      const int nsg = f.segs.empty() ? 1 : (int)f.segs.size();
      for (int d = 0; d < f.ndm; d++)
        for (int l = 0; l < f.nl; l++)
          for (int sg = 0; sg < nsg; sg++)
            for (int a = 0; a < NANT; a++, r++) {
              nr_gpu_chest_row_t &row = cj.rows[r];
              row.rxF = (const int16_t *)rxF.dev + 2 * ((size_t)a * NSYM * N + (size_t)f.dm_sym[d] * N + f.delta(l));
              if (f.segs.empty()) {
                row.pil = (const int16_t *)pil.dev + (size_t)(d * f.nl + l) * 12 * f.pilspan * 2;
                row.out = (int16_t *)est.dev + 2 * ((size_t)(l * NANT + a) * NSYM * N + (size_t)f.dm_sym[d] * N);
                row.k0 = (FCO + 12 * f.start_rb) % N;
                row.nb_rb = f.nrb + f.chest_extra;
              } else { /* nr_pdsch_gpu_chest_grant's segmented rows: scratch rows, composed on the host */
                const SegDef &g = f.segs[(size_t)sg];
                row.pil = (const int16_t *)pil.dev + (size_t)(d * f.nl + l) * 12 * f.pilspan * 2 + 2 * (size_t)f.ppr() * (g.prb_start + (f.refpt ? 0 : f.bwp));
                row.out = (int16_t *)sc.dev + 2 * (size_t)r * N;
                row.k0 = (FCO + 12 * (f.bwp + g.prb_start)) % N;
                row.nb_rb = g.n_prb;
              }
            }
      /* demod: hold the nearest DM-RS estimate; geometry = the grant's contiguous REs */
      int16_t *gy = (int16_t *)geo.host, *gh = gy + 2 * NSYM * ext;
      for (int sy : f.data_syms)
        for (int i = 0; i < len; i++) {
          gy[2 * (sy * ext + i)] = (int16_t)((FCO + 12 * (f.start_rb + f.bwp) + i) % N); /* rxdataF index; estimates are indexed from the grant's first RE (segmented: the virtual allocation at PRB 0) */
          gy[2 * (sy * ext + i) + 1] = 1;
          gh[2 * (sy * ext + i)] = (int16_t)i;
          gh[2 * (sy * ext + i) + 1] = (int16_t)((sy < 7 || f.ndm < 2 ? f.dm_sym[0] : f.dm_sym[1]) + 1);
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
      dj.rxdataF = (const int16_t *)(f.segs.empty() ? rxF.dev : vrx.dev); dj.est = (const int16_t *)est.dev;
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
      for (int d = 0; d < f.ndm; d++)
        for (int l = 0; l < f.nl; l++)
          memcpy((int16_t *)pil.host + (size_t)(d * f.nl + l) * 12 * f.pilspan * 2, f.pilot[d][l].data(), f.pilot[d][l].size() * 2);
      nr_gpu_buf_h2d_async(&pil, 0, pil.bytes, s);
      return cudaStreamSynchronize(s) == cudaSuccess && !cudaMemset(rxF.dev, 0, rxsz) && !cudaMemset(est.dev, 0, est.bytes)
             && !cudaMemset(nvar.dev, 0, nvar.bytes) && !cudaMemset(llrb.dev, 0, llrb.bytes) && !cudaMemset(bits.dev, 0, bits.bytes);
    }
    int stage(int *fs, int *cs) { return m->fep_stage(&fj, fs) || m->chest_stage(&cjv[0], nrows, cs); }
    /* the launch-only part, exactly what a graph captures (the LDPC rc is returned through *ldpc_rc; below CC 9 the non-cluster kernels run, so 0 is the only success) */
    int enqueue_dev(int fs, int cs, cudaStream_t st, int *ldpc_rc)
    {
      const int e1 = m->fep(&fj, &rxF, fs, st), e2 = m->chest(&cjv[0], nrows, &est, &nvar, cs, st), e3 = m->demod(&dj, &llrb, st);
      if (e1 || e2 || e3) { fprintf(stderr, "enqueue_dev: fep %d chest %d demod %d (slots %d %d) last=%s\n", e1, e2, e3, fs, cs, cudaGetErrorString(cudaPeekAtLastError())); return -1; }
      *ldpc_rc = m->ldpc(&lj, 1, &lin, &bits, st);
      return *ldpc_rc == 0 ? 0 : -1;
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

  /* 0 = all four stages ran (ldpc_rc is the LDPC stage's own result); the int16 LLRs of the grant in nr_rx_pdsch order (symbol,
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

  /* ====================================================== Task 4: whole-pass fixtures ======================================================
   * from_shape(): a synthetic grant of a shape class carrying a REAL transport block (CRC, segmentation, OAI LDPC encoder from ./libldpc.so, rate matching,
   * interleaving, scrambling), so the decode succeeds and the TB bytes can be compared. run_stage_path(): the same slot through the Task 2 stages
   * (exact FEP, chest, demod, the GPU LDPC) with the steps between them on the CPU (nvar, descrambling, K38 norm shift, the real OAI rate dematching,
   * the OAI stop rule and CRC) = today's exact path. job: the input of nr_gpu_pass_dl_try. */
  struct TbInfo {
    int A = 0, BG = 2, C = 1, K = 0, Z = 0, F = 0, Kb = 0, rv = 0;
    uint32_t G = 0, E = 0, E2 = 0, first_rE2 = 1;
    uint8_t R = 0, R2 = 0;
    std::vector<uint8_t> payload; /* A / 8 bytes */
    std::vector<uint32_t> scr;    /* gold words */
    std::vector<std::vector<uint8_t>> cblk; /* the code blocks that were encoded (debug) */
    uint32_t rnti = 0x1234, nid = 0x2a5;
  };
  TbInfo tb;
  std::vector<int16_t> rxF_out;           /* the caller's rxdataF of the pass (job.rxdataF) */
  static constexpr uint32_t RXSTRIDE = NSYM * N + 1024;
  nr_gpu_pass_job_t *job = nullptr;       /* heap: large; rebuilt by bind() */
  nr_gpu_pass_shape_t shape = {};
  std::shared_ptr<nr_gpu_pass_job_t> job_sp;

  static std::vector<uint32_t> gold_words(uint32_t cinit, int words)
  {
    /* 38.211 5.2.1: c(n) = x1(n + 1600) ^ x2(n + 1600); x1 = 1, x2 = cinit (LSB first) */
    uint32_t x1 = 1, x2 = cinit & 0x7fffffffu;
    auto step = [&]() {
      const uint32_t b1 = (x1 ^ (x1 >> 3)) & 1u, b2 = (x2 ^ (x2 >> 1) ^ (x2 >> 2) ^ (x2 >> 3)) & 1u;
      const uint32_t out = (x1 ^ x2) & 1u;
      x1 = (x1 >> 1) | (b1 << 30);
      x2 = (x2 >> 1) | (b2 << 30);
      return out;
    };
    for (int i = 0; i < 1600; i++) step();
    std::vector<uint32_t> w((size_t)words, 0);
    for (int i = 0; i < words * 32; i++) w[i >> 5] |= step() << (i & 31);
    return w;
  }
  static int get_E(uint32_t G, int C, int Qm, int Nl, int r)
  {
    if (r <= C - (int)((G / (Nl * Qm)) % C) - 1) return Nl * Qm * (G / (Nl * Qm * C));
    return Nl * Qm * (G / (Nl * Qm * C) + 1);
  }
  static int get_R(int rv, int E, int BG, int Z, int *llrLen, int round)
  {
    static const int k0[2][4] = {{0, 17, 33, 56}, {0, 13, 25, 43}};
    const int Ncb = BG == 1 ? 66 * Z : 50 * Z;
    int info = k0[BG - 1][rv] * Z + E;
    if (round == 0) *llrLen = info;
    if (info > Ncb) info = Ncb;
    if (info > *llrLen) *llrLen = info;
    const int sys = BG == 1 ? 22 * Z : 10 * Z;
    const float r = (float)sys / (info + 2 * Z);
    if (BG == 2) return r < 0.3333 ? 15 : r < 0.6667 ? 13 : 23;
    return r < 0.6667 ? 13 : r < 0.8889 ? 23 : 89;
  }
  /* nr_segmentation() of PHY/CODING/nr_segmentation.c (38.212 5.2.2), without its LOG calls (the test has no log system): C, K, Z, F, Kb of a B-bit TB with CRC;
   * with `in`/`out`, also the code block bytes (CRC24B added for C > 1, filler zeros). */
  static int segment(const uint8_t *in, uint8_t **out, unsigned B, unsigned *C, unsigned *K, unsigned *Zout, unsigned *F, int BG)
  {
    const unsigned Kcb = BG == 1 ? 8448 : 3840;
    unsigned L, Bprime;
    if (B <= Kcb) { L = 0; *C = 1; Bprime = B; }
    else {
      L = 24;
      *C = B / (Kcb - L);
      if ((Kcb - L) * (*C) < B) *C = *C + 1;
      Bprime = B + (*C) * L;
    }
    const unsigned Kprime = Bprime / (*C);
    unsigned Kb = BG == 1 ? 22 : B > 640 ? 10 : B > 560 ? 9 : B > 192 ? 8 : 6;
    unsigned Z = Kprime % Kb ? Kprime / Kb + 1 : Kprime / Kb, k;
    if (Z <= 2) k = 2;
    else if (Z <= 16) k = Z;
    else if (Z <= 32) { k = (Z >> 1) << 1; if (k < Z) k += 2; }
    else if (Z <= 64) { k = (Z >> 2) << 2; if (k < Z) k += 4; }
    else if (Z <= 128) { k = (Z >> 3) << 3; if (k < Z) k += 8; }
    else if (Z <= 256) { k = (Z >> 4) << 4; if (k < Z) k += 16; }
    else if (Z <= 384) { k = (Z >> 5) << 5; if (k < Z) k += 32; }
    else return -1;
    *Zout = k;
    *K = k * (BG == 1 ? 22 : 10);
    *F = *K - Kprime;
    if (in && out) {
      unsigned s = 0;
      for (unsigned r = 0; r < *C; r++) {
        memcpy(out[r], in + s, (Kprime - L) >> 3);
        s += (Kprime - L) >> 3;
        if (*C > 1) {
          const uint32_t crc = crc24b(out[r], Kprime - L) >> 8;
          out[r][(Kprime - L) >> 3] = ((const uint8_t *)&crc)[2];
          out[r][1 + ((Kprime - L) >> 3)] = ((const uint8_t *)&crc)[1];
          out[r][2 + ((Kprime - L) >> 3)] = ((const uint8_t *)&crc)[0];
        }
        for (unsigned q = Kprime >> 3; *F > 0 && q < (*K) >> 3; q++) out[r][q] = 0;
      }
    }
    return (int)Kb;
  }
  /* TB parameters of a grant (G from its geometry) at a target code rate with at most 8 code blocks (one encoder macro group) */
  static bool tb_params(TbInfo *t, int nrb, int nl, int qm, int seed)
  {
    t->G = (uint32_t)nrb * 12u * (uint32_t)(NSYM - first_sym - n_dm) * qm * nl;
    const double Rt = qm <= 4 ? 0.30 : 0.25;
    int A = force_A > 0 ? force_A : ((int)(t->G * Rt)) & ~7;
    for (;; A = (int)(A * 0.8) & ~7) {
      if (A < 64) return false;
      const double R = (A + 24.0) / t->G;
      t->BG = (A <= 292 || (A <= 3824 && R <= 0.67) || R <= 0.25) ? 2 : 1;
      unsigned C, K, Z, F;
      const int B = lenWithCrc(1, A);
      if (segment(nullptr, nullptr, (unsigned)B, &C, &K, &Z, &F, t->BG) < 0) return false;
      if (C <= (unsigned)max_cb) {
        if (A > 3824) { /* 38.214 5.1.3.2: TBS + 24 is a multiple of 8 C, so that every code block is a whole number of bytes */
          const int A2 = (int)((A + 24 + 8 * C - 1) / (8 * C)) * (int)(8 * C) - 24;
          if (A2 != A) { A = A2; unsigned C2; if (segment(nullptr, nullptr, (unsigned)lenWithCrc(1, A), &C2, &K, &Z, &F, t->BG) < 0 || C2 != C) { A = (int)(A * 0.9) & ~7; continue; } }
        }
        t->A = A; t->C = (int)C; t->K = (int)K; t->Z = (int)Z; t->F = (int)F;
        break;
      }
    }
    std::mt19937 rng(seed * 7919 + 13);
    t->payload.resize(t->A / 8);
    for (auto &b : t->payload) b = (uint8_t)rng();
    t->payload[0] |= 1; /* never an all-zero TB */
    t->E = (uint32_t)get_E(t->G, t->C, qm, nl, 0);
    t->E2 = t->E;
    t->first_rE2 = (uint32_t)t->C;
    int ll = 0;
    t->R = (uint8_t)get_R(t->rv, (int)t->E, t->BG, t->Z, &ll, 0);
    t->R2 = t->R;
    for (int r = 1; r < t->C; r++) {
      const int Er = get_E(t->G, t->C, qm, nl, r);
      if ((uint32_t)Er != t->E) {
        t->E2 = (uint32_t)Er;
        t->R2 = (uint8_t)get_R(t->rv, Er, t->BG, t->Z, &ll, 0);
        t->first_rE2 = (uint32_t)r;
        break;
      }
    }
    t->scr = gold_words(((uint32_t)t->rnti << 15) + t->nid, (int)((t->G + 31) / 32) + 1);
    return true;
  }
  struct LdpcEnc { /* encoder_implemparams_t of nrLDPC_defs.h (LP64 layout) */
    unsigned n_segments, first_seg;
    unsigned char gen_code;
    void *tinput, *tinput_memcpy, *tprep, *tparity, *toutput, *tconcat;
    uint32_t K, Kb, Zc, F;
    uint8_t BG;
    unsigned char *output;
    void *ans;
  };
  /* the coded, scrambled bits of the TB in LLR order; false = no encoder available */
  static bool encode_tb(TbInfo *t, int qm, int nl, std::vector<uint8_t> *bits)
  {
    static void *lib = (logInit(), crcTableInit(), dlopen("./libldpc.so", RTLD_NOW | RTLD_GLOBAL));
    static auto enc = lib ? (int (*)(uint8_t **, uint8_t *, LdpcEnc *))dlsym(lib, "LDPCencoder") : nullptr;
    if (!enc) { fprintf(stderr, "fixture: no LDPCencoder in ./libldpc.so: %s\n", dlerror()); return false; }
    const int A = t->A, B = lenWithCrc(1, A), C = t->C, K = t->K, Z = t->Z, F = t->F;
    std::vector<uint8_t> b((size_t)B / 8 + 16, 0);
    memcpy(b.data(), t->payload.data(), (size_t)A / 8);
    if (A > 3824) {
      const uint32_t crc = crc24a(b.data(), A) >> 8;
      b[A / 8] = ((uint8_t *)&crc)[2]; b[A / 8 + 1] = ((uint8_t *)&crc)[1]; b[A / 8 + 2] = ((uint8_t *)&crc)[0];
    } else {
      const uint32_t crc = crc16(b.data(), A) >> 16;
      b[A / 8] = ((uint8_t *)&crc)[1]; b[A / 8 + 1] = ((uint8_t *)&crc)[0];
    }
    if (!check_crc(b.data(), B, crcType(1, A))) { fprintf(stderr, "fixture: TB CRC convention\n"); return false; }
    std::vector<std::vector<uint8_t>> cs((size_t)C, std::vector<uint8_t>((size_t)K / 8 + 64, 0));
    std::vector<uint8_t *> cp((size_t)C);
    for (int r = 0; r < C; r++) cp[(size_t)r] = cs[(size_t)r].data();
    unsigned c_, k_, z_, f_;
    t->cblk = cs;
    t->Kb = segment(b.data(), cp.data(), (unsigned)B, &c_, &k_, &z_, &f_, t->BG);
    t->cblk = cs;
    alignas(64) static uint8_t d[68 * 384];
    LdpcEnc impp = {};
    impp.n_segments = (unsigned)C; impp.K = (uint32_t)K; impp.Kb = (uint32_t)t->Kb; impp.Zc = (uint32_t)Z; impp.F = (uint32_t)F; impp.BG = (uint8_t)t->BG;
    const uint32_t Emax = std::max(t->E, t->E2);
    std::vector<uint8_t> e(Emax + 64, 0), f((t->E + 127) & ~63u, 0), f2((t->E2 + 127) & ~63u, 0);
    bits->clear();
    for (int g0 = 0; g0 < C; g0 += 8) { /* one encoder macro group = 8 code blocks, bit-sliced */
      memset(d, 0, sizeof(d));
      impp.first_seg = (unsigned)g0;
      enc(cp.data(), d, &impp);
      nr_rate_matching_ldpc(0, (uint8_t)t->BG, (uint16_t)Z, d, e.data(), (uint8_t)C, (uint32_t)F, (uint32_t)(K - F - 2 * Z), (uint8_t)t->rv, Emax);
      nr_interleaving_ldpc(t->E, (uint8_t)qm, e.data(), f.data());
      if (t->E2 != t->E) nr_interleaving_ldpc(t->E2, (uint8_t)qm, e.data(), f2.data());
      for (int r = g0; r < C && r < g0 + 8; r++) {
        const bool first = (uint32_t)r < t->first_rE2;
        const std::vector<uint8_t> &src = first ? f : f2;
        for (uint32_t i = 0; i < (first ? t->E : t->E2); i++) bits->push_back((src[i] >> (r - g0)) & 1u);
      }
    }
    (void)nl;
    if (bits->size() != t->G) { fprintf(stderr, "fixture: coded bits %zu != G %u\n", bits->size(), t->G); return false; }
    for (uint32_t i = 0; i < t->G; i++) (*bits)[i] ^= (t->scr[i >> 5] >> (i & 31)) & 1u;
    return true;
  }
  /* shape classes the default warm set covers (nr_gpu_pass_init(16)): 4 PRB classes x layers 1..4 x Qm 2..8 */
  static std::vector<nr_gpu_pass_shape_t> warm_shapes()
  {
    std::vector<nr_gpu_pass_shape_t> v;
    for (int nrb : {24, 48, 96, 217})
      for (int nl = 1; nl <= 4; nl++)
        for (int qm : {2, 4, 6, 8}) {
          int c = nrb * nl / 12;
          if (c < 1) c = 1;
          v.push_back({(uint16_t)nrb, 13, (uint16_t)nl, (uint16_t)qm, 2, (uint16_t)nr_gpu_pass_ncb_class(c > 32 ? 32 : c), 1});
        }
    return v;
  }
  /* shape.nrb_class is the grant's PRB count here (24/48/96/217 are classes themselves) */
  /* corrupt_from_cb >= 0: the coded bits of code blocks >= corrupt_from_cb are flipped with probability 0.3 (those blocks cannot decode) */
  static GpuPassFixture from_shape(const nr_gpu_pass_shape_t &sh, int seed, int corrupt_from_cb = -1, const std::vector<SegDef> *sd = nullptr, int bwp_start = 0, int ref_point = 0, int rv = 0, int payload_seed = -1)
  {
    TbInfo t;
    t.rv = rv; /* a retransmission: the same transport block (payload_seed) with another redundancy version and its own channel / noise (seed) */
    std::vector<uint8_t> bits;
    const bool ok = tb_params(&t, sh.nrb_class, sh.nl, sh.qm, payload_seed >= 0 ? payload_seed : seed) && encode_tb(&t, sh.qm, sh.nl, &bits);
    if (ok && corrupt_from_cb >= 0) {
      std::mt19937 rng(seed + 99);
      std::uniform_real_distribution<double> U(0, 1);
      const uint32_t from = (uint32_t)corrupt_from_cb < t.first_rE2 ? (uint32_t)corrupt_from_cb * t.E : t.first_rE2 * t.E + ((uint32_t)corrupt_from_cb - t.first_rE2) * t.E2;
      for (uint32_t i = from; i < t.G; i++) if (U(rng) < 0.3) bits[i] ^= 1;
    }
    GpuPassFixture f = small_grant(sh.nrb_class, sh.nl, sh.qm, seed, ok ? &bits : nullptr, sd, bwp_start, ref_point);
    f.tb = t;
    f.tb_ok_built = ok;
    f.shape = sh;
    f.bind();
    return f;
  }
  bool tb_ok_built = false;
  /* Task 5: one pass of this fixture's job through the graph (module m, pass_ready) in the FP64 (fp32 = 0) or FP32 (fp32 = 1) demod class; the final LLRs are kept
   * whatever the CRC verdict. The class must have been warmed with the same bit (shape.fp32 bit 1). */
  struct PassRes {
    int rc = -1;
    bool tb_ok = false, need_cpu = false;
    std::vector<int16_t> llr;
  };
  PassRes run_pass(GpuPassModule &m, int fp32)
  {
    PassRes p;
    job->shape.fp32 = (uint8_t)((shape.fp32 & 1) | (fp32 ? NR_GPU_PASS_FP32_DEMOD : 0));
    job->dj.demod_fp32 = fp32;
    job->llr_out_all = 1;
    std::fill(llr_pass.begin(), llr_pass.end(), (int16_t)0);
    std::vector<uint8_t> tb(tb_info_bytes() + 64, 0);
    nr_gpu_pass_result_t r{};
    r.tb = tb.data();
    p.rc = m.pass_try(job, &r);
    p.tb_ok = p.rc == 0 && r.tb_ok;
    p.need_cpu = p.rc == 0 && r.need_cpu;
    p.llr.assign(llr_pass.begin(), llr_pass.begin() + job->G);
    return p;
  }
  size_t tb_info_bytes() const { return (size_t)tb.A / 8; }
  /* sign agreement of two LLR vectors: a position agrees unless the signs are strictly opposite; it counts when either is nonzero */
  static void count_sign_agreement(const PassRes &a, const PassRes &b, size_t *same, size_t *total)
  {
    for (size_t i = 0; i < a.llr.size() && i < b.llr.size(); i++) {
      if (!a.llr[i] && !b.llr[i]) continue;
      ++*total;
      *same += !((a.llr[i] > 0 && b.llr[i] < 0) || (a.llr[i] < 0 && b.llr[i] > 0));
    }
  }
  /* (re)builds the job from this object's own vectors: call after the fixture has settled at its final address */
  void bind()
  {
    job_sp = std::make_shared<nr_gpu_pass_job_t>();
    job = job_sp.get();
    nr_gpu_pass_job_t &j = *job;
    memset(&j, 0, sizeof(j));
    j.shape = shape;
    for (int a = 0; a < NANT; a++) j.rxdata[a] = ring[a].data();
    j.ring_len = ring_len; j.ring_offset = ring_offset; j.symbol_rot = rot.data();
    rxF_out.assign((size_t)2 * NANT * RXSTRIDE, 0);
    j.rxdataF = rxF_out.data(); j.rx_stride = RXSTRIDE; j.copy_mask = 0x3fff;
    j.nbRx = NANT; j.nl = nl; j.ndmrs = ndm;
    for (int d = 0; d < ndm; d++) j.dmrs_sym[d] = dm_sym[d];
    j.dmrs_type = (uint8_t)dtype;
    for (int l = 0; l < nl; l++) {
      j.delta[l] = delta(l);
      for (int d = 0; d < ndm; d++) j.pil[d][l] = pilot[d][l].data();
    }
    j.nrb = nrb + chest_extra; j.k0 = (FCO + 12 * start_rb) % N; /* the chest band */
    j.shape.nrb_class = (uint16_t)nr_gpu_pass_nrb_class(nrb + chest_extra);
    if (!segs.empty()) {
      std::vector<nr_prb_seg_t> sg0;
      for (const SegDef &g : segs) sg0.push_back({(uint16_t)g.prb_start, (uint16_t)g.n_prb, (uint16_t)g.data_index});
      nr_gpu_pass_seg_fill(&j, sg0.data(), (int)sg0.size(), bwp, refpt, FCO, N); /* the dl.c fill */
      j.seg_pil_nrb = pilspan;
      j.shape.nseg_class = (uint16_t)nr_gpu_pass_nseg_class(j.nseg);
    }
    j.number_symbols = 13; j.nvfix = 0; j.branch = -1; j.fo_first = ndm > 1 ? 0 : -1; j.fo_last = ndm > 1 ? 1 : -1; /* as nr_gpu_pass_dl_run */
    j.dt_d = ndm > 1 ? (1.0e-3 / 2) / 14 * (dm_sym[1] - dm_sym[0]) : 0.0; j.df_d = 30000.0;
    j.G = tb.G; j.scr = tb.scr.data(); j.norm = tb.G >= 64; j.norm_span = nr_llr_norm_span(tb.G, (uint32_t)tb.C);
    j.A = (uint32_t)tb.A; j.C = (uint32_t)tb.C; j.K = (uint32_t)tb.K; j.Z = (uint32_t)tb.Z; j.F = (uint32_t)tb.F;
    j.E = tb.E; j.E2 = tb.E2; j.first_rE2 = tb.first_rE2; j.tbslbrm = 0;
    j.BG = (uint8_t)tb.BG; j.R = tb.R; j.R2 = tb.R2; j.rv = (uint8_t)tb.rv; j.Qm = (uint8_t)qm; j.it_max = 8;
    /* the demod geometry: a Pass fixes it (the fixture's contiguous allocation, nearest DM-RS estimate) */
    geo_buf.assign((size_t)4 * NSYM * ((12 * nrb + 15) & ~15), 0);
    nr_gpu_demod_job_t &d = j.dj;
    const int ext = (12 * nrb + 15) & ~15, len = 12 * nrb;
    d.nbRx = NANT; d.nl = nl; d.qm = qm; d.N = N;
    d.start_symbol = data_syms.front(); d.nb_symbols = 13; d.first_symbol = data_syms.front();
    d.rx_stride = NSYM * N; d.est_stride = NSYM * N; d.buf = ext; d.ext = ext;
    uint32_t off = 0;
    int16_t *gy = geo_buf.data(), *gh = gy + 2 * NSYM * ext;
    for (int sy : data_syms) {
      d.sym_mask |= (uint16_t)(1u << sy);
      d.nre[sy] = (uint16_t)len;
      d.llr_off[sy] = off;
      off += len * qm * nl;
      for (int i = 0; i < len; i++) {
        gy[2 * (sy * ext + i)] = (int16_t)((FCO + 12 * (start_rb + bwp) + i) % N);
        gy[2 * (sy * ext + i) + 1] = 1;
        gh[2 * (sy * ext + i)] = (int16_t)i;
        gh[2 * (sy * ext + i) + 1] = (int16_t)((sy < 7 || ndm < 2 ? dm_sym[0] : dm_sym[1]) + 1);
      }
    }
    if (!segs.empty()) { /* the graph reads the real rxdataF: the virtual allocation's index map composed with the old path's gather (what nr_gpu_pass_dl.c does) */
      std::vector<nr_prb_seg_t> sg;
      for (const SegDef &g : segs) sg.push_back({(uint16_t)g.prb_start, (uint16_t)g.n_prb, (uint16_t)g.data_index});
      geo_real.assign(geo_buf.begin(), geo_buf.end());
      if (nr_gpu_pass_seg_remap_geo(geo_real.data(), ext, d.sym_mask, d.nre, N, (FCO + 12 * bwp) % N, sg.data(), (int)sg.size()) != 0) fprintf(stderr, "fixture: seg remap failed\n");
      gy = geo_real.data();
      gh = gy + 2 * NSYM * ext;
    }
    d.thr_coef = std::pow(10.0, -1.2);
    d.qam_mode = nl >= 2 ? 3 : 0; d.mag_per_layer = 1; d.m3_nvfac = std::pow(10.0, 2.1); /* as Pass::setup */
    j.geo_y = gy; j.geo_h = gh;
    llr_pass.assign(tb.G + 64, 0);
    j.llr_out = llr_pass.data();
    if (is_probe) probe_apply();
  }
  /* ---- v2 Task 2: a layout probe of this grant (code block 0 only, the body's symbol horizon). make_probe() then bind() keeps it; run_stage_path() follows it. */
  bool is_probe = false;
  int probe_last = -1;    /* the horizon (-1 = the whole grant) */
  uint32_t n_decode = 0;  /* 0 = C */
  /* the demod geometry past the horizon (nr_pdsch_gpu_demod_host.c build_job: symbols after last_sym are skipped, the allocation's last one is kept); returns the LLRs */
  static uint32_t horizon(nr_gpu_demod_job_t &d, int last_sym, int qm_, int nl_)
  {
    const int last = d.start_symbol + d.nb_symbols - 1;
    uint32_t off = 0;
    for (int m = d.start_symbol; m <= last && m < NSYM; m++) {
      d.llr_off[m] = off;
      if (last_sym >= 0 && m > last_sym && m != last) { d.sym_mask &= (uint16_t)~(1u << m); d.nre[m] = 0; }
      if (d.sym_mask >> m & 1) off += (uint32_t)d.nre[m] * qm_ * nl_;
    }
    return off;
  }
  void make_probe() { is_probe = true; bind(); }
  void probe_apply()
  {
    nr_gpu_pass_job_t &j = *job;
    n_decode = 1;
    probe_last = -1;
    if (tb.C > 1) { /* nr_pdsch_passive_decode.c PROBE HORIZON: the symbol where the LLRs reach E0 plus one symbol of slack, not before the first DM-RS symbol */
      const uint32_t E0 = (tb.G + tb.C - 1) / tb.C, per_sym = (uint32_t)(12 * nrb * qm * nl);
      uint32_t acc = 0;
      for (int m = j.dj.start_symbol; m < j.dj.start_symbol + j.dj.nb_symbols; m++) {
        if (j.dj.sym_mask >> m & 1) acc += (uint32_t)j.dj.nre[m] * qm * nl;
        if (acc >= E0 + per_sym) { probe_last = m; break; }
      }
      if (probe_last >= 0 && probe_last < dm_sym[0]) probe_last = dm_sym[0];
    }
    j.probe = 1;
    j.n_decode = n_decode;
    j.shape.ncb_class = 1;
    j.G_dem = horizon(j.dj, probe_last, qm, nl);
  }
  std::vector<int16_t> llr_pass;
  std::vector<int16_t> llr_ref;
  std::vector<int16_t> llr_pre, llr_post; /* run_stage_path: the demod output before and after the descrambling (before the K38 shift) */
  /* the LLR statistics loops of passive_decode_body_core (nr_pdsch_passive_decode.c, demod_ok block) over the stage path's LLRs: magnitudes before the
   * descrambling, signs after it (copied, not shared, so a change on either side shows) */
  nr_gpu_llr_stats_t host_llr_stats() const
  {
    nr_gpu_llr_stats_t r;
    memset(&r, 0, sizeof(r));
    const uint32_t G = (uint32_t)llr_pre.size();
    for (uint32_t i = 0; i < G; i += 32) {
      const int v = llr_pre[i] < 0 ? -llr_pre[i] : llr_pre[i];
      r.llr_absum += (uint64_t)v;
      r.llr_n++;
      r.llr_zero += v == 0;
      r.llr_sat += v >= 32767;
      r.llr_clip8 += v > 127;
    }
    for (uint32_t i = 0; i + 3 < G; i += 32) {
      for (uint32_t j = 0; j < 4; j++) {
        const int b = j < 2 ? 0 : 1;
        r.nbit[b]++;
        r.posbit[b] += llr_post[i + j] > 0;
      }
      r.llr_sgnsum += (int64_t)llr_post[i];
      r.llr_pos += llr_post[i] > 0;
    }
    return r;
  }
  std::vector<int16_t> est_all; /* stage path: the whole chest estimate [layer * nbRx + ant][14][N] c16 (segmented: in data order) */
  std::vector<int16_t> est_l0; /* segmented stage path: layer-0 estimates [ant][first, last DM-RS symbol][N] c16 (the pass's brfo_rows layout) */
  typedef struct { int16_t r, i; } c16;
  std::vector<int16_t> geo_buf, geo_real, rxF_ref; /* rxF_ref: the exact FEP of the stage path, [ant][14][N] c16 */
  /* the Task 2 stage path with the CPU steps between the stages = the exact path of today; fills s (tb must hold the TB bytes) */
  int run_stage_path(nr_gpu_pass_result_t *s)
  {
    bool ok = false;
    GpuPassModule &m = GpuPassModule::get(&ok);
    if (!ok) return -1;
    Pass P;
    if (!P.setup(m, *this, -1) || !P.load(*this)) return -1;
    cudaStream_t st = P.s;
    int fs = 0, cs = 0;
    const bool seg = !segs.empty();
    constexpr int CH = 256; /* more rows than one chest launch of this process takes: chunks, as gch_flush chunks the production job (every row is independent) */
    if (P.nrows <= CH) {
      if (P.stage(&fs, &cs) || m.fep(&P.fj, &P.rxF, fs, st) || m.chest(&P.cjv[0], P.nrows, seg ? &P.sc : &P.est, &P.nvar, cs, st)) return -1;
      nr_gpu_buf_d2h_async(&P.nvar, 0, P.nvar.bytes, st);
    } else {
      if (m.fep_stage(&P.fj, &fs) || m.fep(&P.fj, &P.rxF, fs, st)) return -1;
      std::vector<int> acc(4 * NR_GPU_CHEST_MAX_ROWS, 0);
      auto jc = std::make_unique<nr_gpu_chest_job_t>(P.cjv[0]);
      for (int r0 = 0; r0 < P.nrows; r0 += CH) {
        const int n = std::min(CH, P.nrows - r0);
        memcpy(jc->rows, P.cjv[0].rows + r0, (size_t)n * sizeof(nr_gpu_chest_row_t));
        jc->n_rows = n;
        if (m.chest_stage(jc.get(), n, &cs) || m.chest(jc.get(), n, seg ? &P.sc : &P.est, &P.nvar, cs, st)) return -1;
        nr_gpu_buf_d2h_async(&P.nvar, 0, P.nvar.bytes, st);
        if (cudaStreamSynchronize(st)) return -1;
        memcpy(&acc[3 * (size_t)r0], P.nvar.host, (size_t)3 * n * sizeof(int));
        memcpy(&acc[3 * (size_t)NR_GPU_CHEST_MAX_ROWS + r0], (const int *)P.nvar.host + 3 * NR_GPU_CHEST_MAX_ROWS, (size_t)n * sizeof(int));
      }
      memcpy(P.nvar.host, acc.data(), acc.size() * sizeof(int));
    }
    nr_gpu_buf_d2h_async(&P.rxF, 0, P.rxsz, st);
    nr_gpu_buf_d2h_async(&P.est, 0, P.est.bytes, st);
    if (seg) nr_gpu_buf_d2h_async(&P.sc, 0, P.sc.bytes, st);
    if (cudaStreamSynchronize(st)) return -1;
    rxF_ref.assign((int16_t *)P.rxF.host, (int16_t *)P.rxF.host + 2 * (size_t)NANT * NSYM * N);
    /* nr_pdsch_gpu_chest_grant's nvar, then the body's normalisation (number_symbols x Nl x nbRx, no per-branch) */
    const int *q = (const int *)P.nvar.host + 3 * NR_GPU_CHEST_MAX_ROWS;
    uint32_t nvar = 0;
    const int ns = seg ? (int)segs.size() : 1;
    memset(s->nvar_ant, 0, sizeof(s->nvar_ant));
    for (int k = 0; k < ndm * nl; k++) {
      /* contiguous: the mean over the antennas with an estimate. Segmented: the passive loop's width-weighted combination of the segments' means (and per antenna) */
      uint64_t nv = 0, nva[NANT] = {0, 0, 0, 0};
      for (int sg = 0; sg < ns; sg++) {
        const int *qq = q + (size_t)(k * ns + sg) * NANT;
        uint64_t acc = 0; int cnt = 0;
        for (int a = 0; a < NANT; a++) if (qq[a] > 0) { acc += (uint32_t)qq[a]; cnt++; }
        const uint32_t mean = cnt ? (uint32_t)(acc / cnt) : 0u;
        const uint64_t w = seg ? (uint64_t)segs[(size_t)sg].n_prb : 1;
        nv += mean * w;
        for (int a = 0; a < NANT; a++) nva[a] += (uint64_t)(uint32_t)qq[a] * w;
      }
      const uint64_t div = seg ? (uint64_t)nrb : 1;
      nvar += (uint32_t)(nv / div);
      for (int a = 0; a < NANT; a++) s->nvar_ant[a] = (uint32_t)(nva[a] / div);
    }
    nvar /= (uint32_t)(13 * nl * NANT);
    if (seg) { /* the old path: each segment's estimate parked at its data offset in a zeroed row (host), and the RE gather into the virtual allocation the demod reads (host) */
      c16 *e = (c16 *)P.est.host;
      const int d0 = dm_sym[0], d1 = dm_sym[1];
      memset(e, 0, P.est.bytes);
      for (int k = 0; k < ndm; k++) /* 1 or 2 DM-RS symbols (rows of the chest job) */
        for (int l = 0; l < nl; l++)
          for (int a = 0; a < NANT; a++)
            for (int sg = 0; sg < ns; sg++) {
              const c16 *src = (const c16 *)P.sc.host + (size_t)(((k * nl + l) * ns + sg) * NANT + a) * N;
              memcpy(e + (size_t)(l * NANT + a) * NSYM * N + (size_t)(k ? d1 : d0) * N + (size_t)segs[(size_t)sg].data_index * 12, src, (size_t)segs[(size_t)sg].n_prb * 12 * 4);
            }
      est_l0.assign((size_t)NANT * 2 * N * 2, 0);
      for (int a = 0; a < NANT; a++) {
        memcpy(&est_l0[((size_t)a * 2) * N * 2], e + (size_t)a * NSYM * N + (size_t)d0 * N, (size_t)N * 4);
        memcpy(&est_l0[((size_t)a * 2 + 1) * N * 2], e + (size_t)a * NSYM * N + (size_t)d1 * N, (size_t)N * 4);
      }
      c16 *vr = (c16 *)P.vrx.host;
      const c16 *rr = (const c16 *)P.rxF.host;
      memset(vr, 0, P.vrx.bytes);
      for (int a = 0; a < NANT; a++)
        for (int sym = 0; sym < NSYM; sym++)
          for (const SegDef &g : segs)
            for (int i = 0; i < g.n_prb * 12; i++)
              vr[(size_t)(a * NSYM + sym) * N + (FCO + bwp * 12 + g.data_index * 12 + i) % N] = rr[(size_t)(a * NSYM + sym) * N + (FCO + bwp * 12 + g.prb_start * 12 + i) % N];
      nr_gpu_buf_h2d_async(&P.est, 0, P.est.bytes, st);
      nr_gpu_buf_h2d_async(&P.vrx, 0, P.vrx.bytes, st);
      if (cudaStreamSynchronize(st)) return -1;
    }
    est_all.assign((const int16_t *)P.est.host, (const int16_t *)P.est.host + P.est.bytes / 2); /* [layer][ant][14][N] c16: every layer, for the DM-RS SNR reference */
    s->nvar = nvar;
    P.dj.nvar = nvar;
    if (is_probe) horizon(P.dj, probe_last, qm, nl); /* the LLRs past the horizon stay 0 (load() cleared the buffer), as the body's cleared buffer */
    if (m.demod(&P.dj, &P.llrb, st)) return -1;
    nr_gpu_buf_d2h_async(&P.llrb, 0, P.llrb.bytes, st);
    if (cudaStreamSynchronize(st)) return -1;
    const uint32_t G = tb.G;
    std::vector<int16_t> llr((int16_t *)P.llrb.host, (int16_t *)P.llrb.host + G);
    if (getenv("GP_DEBUG")) {
      size_t err = 0;
      for (uint32_t i = 0; i < G; i++) err += (llr[i] < 0) != (tx_bits[i] != 0);
      fprintf(stderr, "GP_DEBUG raw BER %.5f (%zu / %u) nvar %u C %d E %u E2 %u firstE2 %u R %d R2 %d K %d Z %d F %d\n", (double)err / G, err, G, nvar, tb.C, tb.E, tb.E2, tb.first_rE2, tb.R, tb.R2, tb.K, tb.Z, tb.F);
    }
    llr_pre = llr;
    for (uint32_t i = 0; i < G; i++) if (tb.scr[i >> 5] >> (i & 31) & 1u) llr[i] = (int16_t)(-llr[i]);
    llr_post = llr;
    if (G >= 64) {
      const int k = nr_llr_norm_shift_nl(llr.data(), nr_llr_norm_span(G, (uint32_t)tb.C), nl, qm);
      if (k > 0) for (uint32_t i = 0; i < G; i++) llr[i] = (int16_t)(llr[i] >> k);
    }
    return decode_llr(m, P, st, llr, s);
  }

  /* the OAI rate dematching, the GPU LDPC (Task 2 stage path) and the OAI stop rule + CRC on the final LLRs of a grant (descrambled, normalised as the caller wants);
   * s receives seg_ok / need_cpu / the TB / tb_ok. P: a Pass whose lin / bits buffers are used. */
  int decode_llr(GpuPassModule &m, Pass &P, cudaStream_t st, std::vector<int16_t> &llr, nr_gpu_pass_result_t *s)
  {
    const uint32_t G = tb.G;
    (void)G;
    llr_ref = llr; /* what the retry arms of the old path re-decode */
    /* the OAI rate dematching and the pack of prep_segment (nr_pdsch_gpu_ldpc_host.c) */
    const int Ctb = tb.C, Z = tb.Z, K = tb.K, F = tb.F, Kc = tb.BG == 2 ? 52 : 68;
    const int Kprime = lenWithCrc(Ctb, tb.A), ctype = crcType(Ctb, tb.A);
    const int C = n_decode && (int)n_decode < Ctb ? (int)n_decode : Ctb; /* a probe decodes block 0 with the whole TB's parameters (nb_segments_to_decode) */
    std::vector<nr_gpu_ldpc_job_t> jobs((size_t)C);
    std::vector<int16_t> e(std::max(tb.E, tb.E2) + 16), d((size_t)Kc * Z + 16), zv((size_t)68 * 384 + 16);
    std::vector<int8_t> lin_all((size_t)C * NR_GPU_LDPC_DEV_IN_STRIDE, 0);
    for (int r = 0; r < C; r++) {
      const bool first = (uint32_t)r < tb.first_rE2;
      const uint32_t E = first ? tb.E : tb.E2;
      const uint32_t off = first ? (uint32_t)r * tb.E : tb.first_rE2 * tb.E + ((uint32_t)r - tb.first_rE2) * tb.E2;
      std::fill(d.begin(), d.end(), 0);
      nr_deinterleaving_ldpc(E, (uint8_t)qm, e.data(), llr.data() + off);
      if (nr_rate_matching_ldpc_rx(0, (uint8_t)tb.BG, (uint16_t)Z, d.data(), e.data(), (uint8_t)Ctb, (uint8_t)tb.rv, 1, E, (uint32_t)F, (uint32_t)(K - F - 2 * Z)) == -1) return -1;
      std::fill(zv.begin(), zv.end(), 0);
      memset(zv.data() + Kprime, 127, (size_t)F * 2);
      memcpy(zv.data() + 2 * Z, d.data(), (size_t)(Kprime - 2 * Z) * 2);
      memcpy(zv.data() + K, d.data() + (K - 2 * Z), (size_t)(Kc * Z - K) * 2);
      int8_t *l = lin_all.data() + (size_t)r * NR_GPU_LDPC_DEV_IN_STRIDE;
      for (int i = 0; i < Kc * Z; i++) { const int v = zv[(size_t)i]; l[i] = (int8_t)(v > 127 ? 127 : v < -128 ? -128 : v); }
      jobs[(size_t)r] = {};
      jobs[(size_t)r].BG = (uint8_t)tb.BG; jobs[(size_t)r].R = first ? tb.R : tb.R2; jobs[(size_t)r].Z = (uint16_t)Z; jobs[(size_t)r].K = (uint16_t)K; jobs[(size_t)r].it_max = 8;
    }
    /* the standalone context takes NR_GPU_LDPC_MAX_JOBS blocks per launch (a TB with more is cut into launches, as nr_pdsch_gpu_ldpc_host.c does) */
    std::vector<uint8_t> snap_all((size_t)C * NR_GPU_LDPC_MAX_SNAP * NR_GPU_LDPC_KB_MAX);
    std::vector<int> res_all((size_t)2 * C);
    for (int r0 = 0; r0 < C; r0 += NR_GPU_LDPC_MAX_JOBS) {
      const int n = std::min(C - r0, (int)NR_GPU_LDPC_MAX_JOBS);
      memcpy(P.lin.host, lin_all.data() + (size_t)r0 * NR_GPU_LDPC_DEV_IN_STRIDE, (size_t)n * NR_GPU_LDPC_DEV_IN_STRIDE);
      nr_gpu_buf_h2d_async(&P.lin, 0, (size_t)(n - 1) * NR_GPU_LDPC_DEV_IN_STRIDE + (size_t)Kc * Z, st);
      if (m.ldpc(jobs.data() + r0, n, &P.lin, &P.bits, st)) return -1;
      nr_gpu_buf_d2h_async(&P.bits, 0, P.bits.bytes, st);
      if (cudaStreamSynchronize(st)) return -1;
      const int *rs = (const int *)((char *)P.bits.host + NR_GPU_LDPC_DEV_RES_OFF);
      for (int r = 0; r < n; r++) {
        res_all[(size_t)2 * (r0 + r)] = rs[2 * r];
        res_all[(size_t)2 * (r0 + r) + 1] = rs[2 * r + 1];
        memcpy(snap_all.data() + (size_t)(r0 + r) * NR_GPU_LDPC_MAX_SNAP * NR_GPU_LDPC_KB_MAX, (uint8_t *)P.bits.host + (size_t)r * NR_GPU_LDPC_MAX_SNAP * NR_GPU_LDPC_KB_MAX,
               (size_t)NR_GPU_LDPC_MAX_SNAP * NR_GPU_LDPC_KB_MAX);
      }
    }
    const int *res = res_all.data();
    const int KB = (K + 7) >> 3; /* a lifting size that is not a multiple of 4 gives K = Kb * Z of no whole number of bytes */
    if (getenv("GP_DEBUG"))
      for (int r = 0; r < C; r++) {
        int diff = -1;
        if (res[2 * r] > 0) {
          diff = 0;
          const uint8_t *sn = snap_all.data() + ((size_t)r * NR_GPU_LDPC_MAX_SNAP) * NR_GPU_LDPC_KB_MAX;
          for (int q2 = 0; q2 < K / 8; q2++) diff += __builtin_popcount((unsigned)(sn[q2] ^ tb.cblk[(size_t)r][(size_t)q2]));
        }
        fprintf(stderr, "GP_DEBUG cb %d nsnap %d syn0 %d first-snapshot bit diff vs encoded block %d (Kprime %d)\n", r, res[2 * r], res[2 * r + 1], diff, Kprime);
      }
    std::vector<uint8_t> c((size_t)C * KB, 0);
    bool aborted = false, all = true;
    s->need_cpu = 0;
    memset(s->seg_ok, 0, sizeof(s->seg_ok));
    for (int r = 0; r < C; r++) {
      int hit = -1;
      if (!aborted)
        for (int q2 = 0; q2 < res[2 * r] && hit < 0; q2++)
          if (check_crc(snap_all.data() + ((size_t)r * NR_GPU_LDPC_MAX_SNAP + q2) * NR_GPU_LDPC_KB_MAX, Kprime, ctype)) hit = q2;
      if (hit >= 0) {
        memcpy(c.data() + (size_t)r * KB, snap_all.data() + ((size_t)r * NR_GPU_LDPC_MAX_SNAP + hit) * NR_GPU_LDPC_KB_MAX, KB);
        s->seg_ok[r] = 1;
        continue;
      }
      if (!aborted && res[2 * r + 1]) { s->need_cpu = 1; }
      all = false;
      aborted = true;
    }
    s->tbs = (uint32_t)tb.A;
    s->tb_ok = 0;
    if (!s->need_cpu) {
      const int seg_bytes = ((K - F) >> 3) - (Ctb > 1 ? 3 : 0);
      uint32_t off = 0;
      for (int r = 0; r < C; r++, off += seg_bytes) memcpy(s->tb + off, c.data() + (size_t)r * KB, seg_bytes);
      s->tb_ok = all && C == Ctb && (C == 1 || check_crc(s->tb, lenWithCrc(1, tb.A), crcType(1, tb.A)));
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
