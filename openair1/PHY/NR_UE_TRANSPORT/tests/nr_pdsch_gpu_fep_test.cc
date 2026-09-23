/* Standalone check + benchmark of libpdsch_gpu.so: synthesises a 273-PRB / 4-antenna slot (own DM-RS
 * generator, QAM data, frequency-selective 4xNl channel, OAI TX symbol rotation, FO, int16 quantisation,
 * ring wrap), runs the GPU FEP + chest + MMSE + LLR and scores the LLR hard decisions against the
 * transmitted bits; then times FEP per slot and chest+LLR per probe at batch 1/32/256.
 * Usage: nr_pdsch_gpu_fep_test [path/to/libpdsch_gpu.so]  (exit 0 = pass) */
#define NR_GPU_FEP_NO_LOADER
#include "nr_pdsch_gpu_fep.h"
#include <dlfcn.h>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
#include <chrono>

typedef std::complex<double> cd;
static const int N = 4096, NCP = 288, NCP0 = 352, NSYM = 14, NRB = 273, FCO = N - NRB * 6, NANT = 4, FS = 122880000;
static const int SLOT = NCP0 + N + (NSYM - 1) * (N + NCP);
static const double F0 = 3.75e9;

/* 38.211 5.2.1 gold, bit n of c */
static std::vector<uint8_t> gold(uint32_t cinit, int nbits)
{
  std::vector<uint8_t> x1(1600 + nbits + 31), x2(1600 + nbits + 31), c(nbits);
  x1[0] = 1;
  for (int i = 0; i < 31; i++)
    x2[i] = (cinit >> i) & 1;
  for (int n = 0; n + 31 < (int)x1.size(); n++) {
    x1[n + 31] = x1[n + 3] ^ x1[n];
    x2[n + 31] = x2[n + 3] ^ x2[n + 2] ^ x2[n + 1] ^ x2[n];
  }
  for (int n = 0; n < nbits; n++)
    c[n] = x1[n + 1600] ^ x2[n + 1600];
  return c;
}
static double pam(int qm, int code)
{
  /* 38.211 5.1.x written out per level so the GPU's recursion is checked against a different form */
  const int h = qm / 2;
  const int b[4] = {code & 1, (code >> 1) & 1, (code >> 2) & 1, (code >> 3) & 1}; /* b[i] = b_{2i} of the RE */
  double a;
  if (h == 1) a = 1;
  else if (h == 2) a = 2 - (1 - 2 * b[1]);
  else if (h == 3) a = 4 - (1 - 2 * b[1]) * (2 - (1 - 2 * b[2]));
  else a = 8 - (1 - 2 * b[1]) * (4 - (1 - 2 * b[2]) * (2 - (1 - 2 * b[3])));
  a *= 1 - 2 * b[0];
  return a / sqrt(qm == 2 ? 2 : qm == 4 ? 10 : qm == 6 ? 42 : 170);
}
static int dmrs_delta(int type, int port) { return type == 1 ? ((port >> 1) & 1) : 2 * ((port >> 1) % 3); }
static int is_data(int type, int ngrp, int kr) { return type == 1 ? (ngrp >= 2 ? 0 : (kr & 1)) : ((kr % 6) >= 2 * ngrp); }

struct Scene {
  std::vector<int16_t> ring[NANT];
  uint32_t ring_len, ring_offset;
  std::vector<int16_t> rot; /* 14 c16 */
  std::vector<uint8_t> bits; /* in LLR order */
  double fo_hz;
};

/* Build the slot for one job; returns TX bits in nr_rx_pdsch LLR order. */
static double g_cross = 1.0; static int g_maxdly = 19; /* gain of the a != l channel entries: 1 = iid (ill-conditioned at rank 4), 0.3 = well conditioned */
static Scene make_scene(const nr_gpu_pdsch_job_t &j, double snr_db, double fo_hz, std::mt19937 &rng)
{
  Scene sc;
  sc.fo_hz = fo_hz;
  std::uniform_real_distribution<double> U(0, 1);
  std::normal_distribution<double> G(0, 1);
  const int Nl = j.Nl;
  /* channel: 3 taps per (ant, layer), delays <= 20 samples */
  std::vector<cd> taps[NANT][4];
  std::vector<int> dly[NANT][4];
  for (int a = 0; a < NANT; a++)
    for (int l = 0; l < Nl; l++)
      for (int t = 0; t < 3; t++) {
        taps[a][l].push_back(cd(G(rng), G(rng)) * (t == 0 ? 1.0 : 0.4) * (a == l ? 1.0 : g_cross));
        dly[a][l].push_back(t == 0 ? 0 : 1 + (int)(U(rng) * g_maxdly));
      }
  auto H = [&](int a, int l, int k /* signed subcarrier */) {
    cd h = 0;
    for (size_t t = 0; t < taps[a][l].size(); t++)
      h += taps[a][l][t] * std::polar(1.0, -2 * M_PI * k * dly[a][l][t] / N);
    return h;
  };
  /* frequency grid per antenna */
  std::vector<cd> Y[NANT];
  for (int a = 0; a < NANT; a++)
    Y[a].assign((size_t)NSYM * N, 0);
  /* DM-RS */
  for (int l = 0; l < NSYM; l++) {
    if (!((j.dmrs_mask >> l) & 1))
      continue;
    const uint32_t cinit = ((1u << 17) * (14u * j.slot + l + 1) * (2u * j.dmrs_scrambling_id + 1) + 2u * j.dmrs_scrambling_id + j.nscid) & 0x7fffffffu;
    std::vector<uint8_t> c = gold(cinit, 2 * 6 * (NRB + 2));
    const int ppr = j.dmrs_type == 1 ? 3 : 2;
    for (int li = 0; li < Nl; li++) {
      const int port = j.ports[li], delta = dmrs_delta(j.dmrs_type, port), wf = (port & 1) ? -1 : 1;
      for (int rb = 0; rb < j.nb_rb; rb++)
        for (int n = 0; n < ppr; n++) {
          const int m0 = 2 * (ppr * (j.start_rb + rb - j.dmrs_ref_rb) + n);
          for (int kp = 0; kp < 2; kp++) {
            const cd r((1 - 2 * c[2 * (m0 + kp)]) / sqrt(2), (1 - 2 * c[2 * (m0 + kp) + 1]) / sqrt(2));
            const int kr = j.dmrs_type == 1 ? 4 * n + 2 * kp + delta : 6 * n + kp + delta;
            const int k = 12 * (j.start_rb + rb) + kr; /* CRB0-relative */
            const int ks = k - NRB * 6;                /* signed */
            const int idx = (FCO + k) % N;
            for (int a = 0; a < NANT; a++)
              Y[a][(size_t)l * N + idx] += H(a, li, ks) * r * (double)(kp ? wf : 1) * sqrt((double)j.n_cdm_groups_no_data); /* 38.214 4.1-1 DM-RS boost */
          }
        }
    }
  }
  /* data in LLR order: symbol, RE (increasing k), layer, bit */
  for (int l = j.start_symbol; l < j.start_symbol + j.nb_symbols; l++) {
    const int isd = (j.dmrs_mask >> l) & 1;
    for (int kr_all = 0; kr_all < 12 * j.nb_rb; kr_all++) {
      if (isd && !is_data(j.dmrs_type, j.n_cdm_groups_no_data, kr_all % 12))
        continue;
      const int k = 12 * j.start_rb + kr_all, ks = k - NRB * 6, idx = (FCO + k) % N;
      for (int li = 0; li < Nl; li++) {
        int ci = 0, cq = 0;
        for (int b = 0; b < j.Qm; b++) {
          const int bit = U(rng) < 0.5;
          sc.bits.push_back(bit);
          if (b & 1) cq |= bit << (b / 2); else ci |= bit << (b / 2);
        }
        const cd x(pam(j.Qm, ci), pam(j.Qm, cq));
        for (int a = 0; a < NANT; a++)
          Y[a][(size_t)l * N + idx] += H(a, li, ks) * x;
      }
    }
  }
  /* OAI TX symbol rotation table (perform_symbol_rotation) */
  sc.rot.resize(2 * NSYM);
  {
    const double Tc = 1 / 480e3 / 4096, Nu = 2048 * 64 / 2.0, Ncp0 = 16 * 64 + 144 * 64 / 2.0, Ncp1 = 144 * 64 / 2.0;
    double tl = 0;
    for (int l = 0; l < NSYM; l++) {
      const double Ncp = l == 0 ? Ncp0 : Ncp1, poff = 2 * M_PI * (tl + Ncp * Tc) * F0;
      sc.rot[2 * l] = (int16_t)floor(cos(poff) * 32767);
      sc.rot[2 * l + 1] = (int16_t)floor(sin(-poff) * 32767);
      tl += (Nu + Ncp) * Tc;
    }
  }
  /* time domain: IFFT (naive per used subcarrier is too slow -> radix via std: do a plain O(N log N) FFT) */
  auto ifft = [&](std::vector<cd> &v) {
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
  };
  sc.ring_len = 3 * SLOT;
  sc.ring_offset = sc.ring_len - 1000; /* the slot wraps around the ring end */
  const double sig_amp = 40.0; /* per-RE amplitude before the IFFT */
  /* AWGN per RE in the frequency domain (SNR = unit-energy layer symbol vs noise per RE) */
  const double noise_sd = pow(10, -snr_db / 20) / sqrt(2);
  for (int a = 0; a < NANT; a++)
    for (auto &y : Y[a])
      y += cd(G(rng), G(rng)) * noise_sd;
  for (int a = 0; a < NANT; a++) {
    std::vector<double> td(2 * (SLOT + 64), 0.0); /* I,Q with a little slack at the end */
    for (int l = 0; l < NSYM; l++) {
      std::vector<cd> v(Y[a].begin() + (size_t)l * N, Y[a].begin() + (size_t)(l + 1) * N);
      for (auto &x : v) x *= sig_amp;
      ifft(v);
      const cd rot(sc.rot[2 * l] / 32767.0, sc.rot[2 * l + 1] / 32767.0);
      const int ncp = l == 0 ? NCP0 : NCP, start = (l == 0 ? 0 : NCP0 + N + (l - 1) * (N + NCP));
      for (int n = 0; n < ncp + N; n++) {
        const cd s = v[(n - ncp + N) % N] * rot;
        td[2 * (start + n)] = s.real();
        td[2 * (start + n) + 1] = s.imag();
      }
    }
    sc.ring[a].assign((size_t)2 * sc.ring_len, 0);
    for (int n = 0; n < SLOT + 64; n++) {
      const uint32_t abs_i = sc.ring_offset + n; /* FO phase origin = ring_offset (passed as abs_sample) */
      const cd fo = std::polar(1.0, 2 * M_PI * fo_hz * (double)abs_i / FS);
      const cd s = cd(td[2 * n], td[2 * n + 1]) * fo;
      const size_t p = (sc.ring_offset + n) % sc.ring_len;
      sc.ring[a][2 * p] = (int16_t)std::max(-32768.0, std::min(32767.0, round(s.real())));
      sc.ring[a][2 * p + 1] = (int16_t)std::max(-32768.0, std::min(32767.0, round(s.imag())));
    }
  }
  return sc;
}

static const nr_gpu_fep_api_t *api;
static double us_since(std::chrono::steady_clock::time_point t) { return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t).count(); }

static int run_case(const char *name, nr_gpu_pdsch_job_t j, double snr_db, double fo_hz, double max_ber, std::mt19937 &rng)
{
  Scene sc = make_scene(j, snr_db, fo_hz, rng);
  const int16_t *rx[NANT];
  for (int a = 0; a < NANT; a++) rx[a] = sc.ring[a].data();
  if (api->fep_slot(rx, sc.ring_len, sc.ring_offset, sc.ring_offset, fo_hz, sc.rot.data())) { printf("%s: fep failed\n", name); return 1; }
  std::vector<int16_t> llr(sc.bits.size() + 64);
  const int64_t n = api->pdsch_llr(&j, 1, llr.data(), llr.size());
  if (n != (int64_t)sc.bits.size()) { printf("%s: got %lld LLRs, expected %zu\n", name, (long long)n, sc.bits.size()); return 1; }
  size_t err = 0; double mag = 0;
  for (size_t i = 0; i < sc.bits.size(); i++) { err += (llr[i] < 0) != sc.bits[i]; mag += fabs(llr[i]); }
  const double ber = (double)err / sc.bits.size();
  if (getenv("DBG")) { /* where are the errors: per layer, per bit, per symbol (assumes no data on DM-RS symbols) */
    long el[4] = {0}, eb[8] = {0}, es[14] = {0};
    const int per_sym = 12 * j.nb_rb * j.Nl * j.Qm;
    for (size_t i = 0; i < sc.bits.size(); i++)
      if ((llr[i] < 0) != sc.bits[i]) { el[(i / j.Qm) % j.Nl]++; eb[i % j.Qm]++; es[i / per_sym]++; }
    printf("  layer:"); for (int l = 0; l < j.Nl; l++) printf(" %ld", el[l]);
    printf("  bit:"); for (int b = 0; b < j.Qm; b++) printf(" %ld", eb[b]);
    printf("  sym:"); for (int q = 0; q < 14; q++) printf(" %ld", es[q]);
    printf("\n");
  }
  double fep_us, llr_us; api->timing(&fep_us, &llr_us);
  printf("%-34s Nl=%d Qm=%d type=%d cdm=%d fo=%.0f: %zu LLRs, BER %.2e, mean|LLR| %.1f, fep %.0f us, chest+llr %.0f us -> %s\n",
         name, j.Nl, j.Qm, j.dmrs_type, j.n_cdm_groups_no_data, fo_hz, sc.bits.size(), ber, mag / sc.bits.size(), fep_us, llr_us,
         ber <= max_ber ? "OK" : "FAIL");
  return ber <= max_ber ? 0 : 1;
}

int main(int argc, char **argv)
{
  void *h = dlopen(argc > 1 ? argv[1] : "./libpdsch_gpu.so", RTLD_NOW);
  if (!h) { fprintf(stderr, "%s\n", dlerror()); return 2; }
  auto get = (const nr_gpu_fep_api_t * (*)(void)) dlsym(h, "nr_gpu_fep_api");
  if (!get) { fprintf(stderr, "no nr_gpu_fep_api\n"); return 2; }
  api = get();
  nr_gpu_fep_cfg_t cfg = {NANT, N, NCP, NCP0, NSYM, FCO, NRB, FS / 1000, 8};
  if (api->init(&cfg)) { fprintf(stderr, "init failed\n"); return 2; }
  std::mt19937 rng(7);
  int fails = 0;
  nr_gpu_pdsch_job_t base = {};
  base.start_rb = 0; base.nb_rb = NRB; base.start_symbol = 1; base.nb_symbols = 13; base.dmrs_mask = (1 << 2) | (1 << 11);
  base.dmrs_type = 1; base.n_cdm_groups_no_data = 2; base.Nl = 4; base.ports[0] = 0; base.ports[1] = 1; base.ports[2] = 2; base.ports[3] = 3;
  base.Qm = 8; base.dmrs_scrambling_id = 2; base.nscid = 0; base.dmrs_ref_rb = 0; base.slot = 3; base.time_interp = 1;
  /* Rank-4 256QAM over an iid 4x4 channel is MMSE-noise-enhancement limited (measured: BER ~1e-2 at
   * 40 dB even on a flat channel while 16QAM is error-free on the same draws), so it is printed, not
   * scored; the scored rank-4 cases use 16QAM or a dominant-diagonal flat channel. Linear interpolation
   * over a 20-sample delay spread is the other estimation floor (same class as the CPU chest's filters). */
  run_case("rank4 256QAM iid channel dly<=19 (info)", base, 40, 0, 1.0, rng);
  g_cross = 0.3;
  g_maxdly = 0;
  fails += run_case("rank4 256QAM flat dominant-diag 50 dB", base, 50, 0, 1e-4, rng); /* 40 dB: BER ~1e-3 on the weakest layer, 60 dB: 0 */
  g_maxdly = 19;
  { nr_gpu_pdsch_job_t j = base; j.Qm = 4; fails += run_case("rank4 16QAM dly<=19", j, 40, 0, 3e-3, rng); }
  { nr_gpu_pdsch_job_t j = base; j.Qm = 4; j.dmrs_mask = (1 << 2) | (1 << 7) | (1 << 11);
    fails += run_case("rank4 16QAM 3 DM-RS symbols +FO 300", j, 40, 300, 3e-3, rng); }
  g_cross = 1.0;
  { nr_gpu_pdsch_job_t j = base; j.Qm = 6; j.Nl = 2; fails += run_case("rank2 64QAM type1 +FO 1500 Hz", j, 30, 1500, 1e-3, rng); }
  { nr_gpu_pdsch_job_t j = base; j.Qm = 4; j.Nl = 1; j.n_cdm_groups_no_data = 1; j.start_rb = 40; j.nb_rb = 100; j.dmrs_ref_rb = 40;
    fails += run_case("rank1 16QAM type1 cdm1 data on DM-RS", j, 20, -700, 1e-3, rng); }
  { nr_gpu_pdsch_job_t j = base; j.dmrs_type = 2; j.Qm = 2; j.Nl = 2; j.n_cdm_groups_no_data = 1; j.dmrs_mask = 1 << 2; j.time_interp = 0;
    fails += run_case("rank2 QPSK type2 single DM-RS hold", j, 15, 0, 1e-3, rng); }
  { nr_gpu_pdsch_job_t j = base; j.dmrs_type = 2; j.Qm = 6; j.Nl = 4; j.ports[0] = 0; j.ports[1] = 1; j.ports[2] = 2; j.ports[3] = 3;
    j.n_cdm_groups_no_data = 2; j.Qm = 4; fails += run_case("rank4 16QAM type2", j, 40, 0, 3e-3, rng); }

  /* ---- timing: probes (first code block: ~9.6 kbit at MCS 25 rank 4 = 300 REs) and full TBs ---- */
  Scene sc = make_scene(base, 30, 0, rng);
  const int16_t *rx[NANT];
  for (int a = 0; a < NANT; a++) rx[a] = sc.ring[a].data();
  double fep_acc = 0;
  for (int r = 0; r < 20; r++) { api->fep_slot(rx, sc.ring_len, sc.ring_offset, sc.ring_offset, 0, sc.rot.data()); double f, l; api->timing(&f, &l); fep_acc += f; }
  printf("FEP per slot (4 ant x 14 sym x 4096, upload+FFT+rotate): %.0f us\n", fep_acc / 20);
  std::vector<int16_t> out((size_t)64 << 20);
  for (int batch : {1, 32, 256}) {
    std::vector<nr_gpu_pdsch_job_t> jobs(batch, base);
    for (int i = 0; i < batch; i++) { jobs[i].start_rb = (i * 37) % 200; jobs[i].nb_rb = 73 + (i * 11) % 200; jobs[i].max_llr = 9600; }
    api->pdsch_llr(jobs.data(), batch, out.data(), out.size()); /* warm */
    const int reps = 10;
    auto t = std::chrono::steady_clock::now();
    for (int r = 0; r < reps; r++) api->pdsch_llr(jobs.data(), batch, out.data(), out.size());
    const double us = us_since(t) / reps;
    printf("probes (CB0 = 9600 LLRs each) batch %3d: %8.0f us/launch set, %7.1f us/probe, %7.0f probes/s\n", batch, us, us / batch, 1e6 * batch / us);
  }
  for (int batch : {1, 8, 32}) {
    std::vector<nr_gpu_pdsch_job_t> jobs(batch, base); /* full-band rank-4 256QAM TB: 1.36 M LLRs each */
    api->pdsch_llr(jobs.data(), batch, out.data(), out.size());
    const int reps = 5;
    auto t = std::chrono::steady_clock::now();
    for (int r = 0; r < reps; r++) api->pdsch_llr(jobs.data(), batch, out.data(), out.size());
    const double us = us_since(t) / reps;
    printf("full TBs (273 RB, 13 sym, rank 4, 256QAM) batch %2d: %8.0f us/launch set, %7.0f us/TB\n", batch, us, us / batch);
  }
  printf(fails ? "FAILED (%d)\n" : "ALL OK\n", fails);
  return fails ? 1 : 0;
}
