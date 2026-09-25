/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 * Bit-exactness of the GPU SC decoder against polar_decoder_int16(), then throughput.
 *   ./nr_polar_sc_cuda_test            exactness + benchmark
 *   ./nr_polar_sc_cuda_test quick      exactness only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <vector>
#include "nr_polar_sc_cuda.h"
#include "nr_polar_sc_cuda_int.h"

extern "C" {
#include "PHY/CODING/nrPolar_tools/nr_polar_defs.h"
#include "PHY/CODING/nrPolar_tools/nr_polar_dci_defs.h"
uint32_t polar_decoder_int16(int16_t *input, uint64_t *out, uint8_t ones_flag, int8_t messageType,
                             uint16_t messageLength, uint8_t aggregation_level);
void polar_encoder_fast(uint64_t *A, void *out, int32_t crcmask, uint8_t ones_flag, int8_t messageType,
                        uint16_t messageLength, uint8_t aggregation_level);
/* the same two stubs every standalone test in NR_UE_TRANSPORT/tests carries */
struct configmodule_interface_s *uniqCfg = NULL;
void exit_function(const char *, const char *, int, const char *, int) { abort(); }
const uint8_t *npc_dbg_last_u(int i);
int npc_dbg_params(int pid, int *N, int *K, const uint8_t **info, const uint16_t **il);
int npc_ref_decode(const npc_item_t *items, int n, uint32_t *crc, uint64_t *payload);
const uint8_t *npc_ref_last_u(void);
int npc_gpu_dbg_device_params(int *);
int npc_gpu_dbg_fetch(int *);
int npc_gpu_dbg_alpha(int16_t *);
const int16_t *npc_ref_last_alpha(void);
int npc_gpu_dbg_trace(int16_t *);
const int16_t *npc_ref_last_trace(void);
int npc_ref_op(int pid, int k, int *code, int *level, int *fli);
uint32_t crc24c(unsigned char *inptr, int bitlen);
}

static double now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }
static double gauss(void) { double u = (rnd() + 1.0) / 4294967297.0, v = (rnd() + 1.0) / 4294967297.0; return sqrt(-2 * log(u)) * cos(6.283185307 * v); }

struct pset { uint16_t len; uint8_t al; };
static const pset P[] = {{39, 1}, {44, 2}, {47, 2}, {49, 2}, {58, 2}, {47, 4}, {58, 4}, {47, 8}, {58, 8}, {39, 8}, {47, 16}, {58, 16}};
#define NP ((int)(sizeof(P) / sizeof(P[0])))

static int E_of(const pset &p) { return p.al * 108; }

/* One CPU decode of the exact vector the GPU gets; the CPU path must be warmed first (see .h). */
static uint32_t cpu_decode(const pset &p, const int16_t *llr, uint64_t *payload)
{
  int16_t tmp[NPC_MAX_E];
  memcpy(tmp, llr, sizeof(int16_t) * E_of(p));
  uint64_t out[2] = {0, 0};
  const uint32_t crc = polar_decoder_int16(tmp, out, 1, NR_POLAR_DCI_MESSAGE_TYPE, p.len, p.al);
  *payload = out[0];
  return crc;
}

static void encode(const pset &p, uint64_t payload, uint16_t rnti, double amp, double sigma, int16_t *llr)
{
  uint8_t out[NPC_MAX_E / 8 + 16] = {0};
  uint64_t A[2] = {payload, 0};
  polar_encoder_fast(A, out, rnti, 1, NR_POLAR_DCI_MESSAGE_TYPE, p.len, p.al);
  const int E = E_of(p);
  for (int i = 0; i < E; i++) {
    const int bit = (out[i >> 3] >> (i & 7)) & 1;
    double v = (bit ? -amp : amp) + sigma * gauss();
    if (v > 32767) v = 32767; if (v < -32768) v = -32768;
    llr[i] = (int16_t)lrint(v);
  }
}

static int compare(const char *what, const pset &p, int pid, const std::vector<std::vector<int16_t>> &vecs, int *crc_pass)
{
  const int n = (int)vecs.size();
  std::vector<npc_item_t> items(n);
  std::vector<uint32_t> gcrc(n); std::vector<uint64_t> gpay(n);
  for (int i = 0; i < n; i++) items[i] = (npc_item_t){pid, vecs[i].data()};
  if (npc_decode_batch(items.data(), n, gcrc.data(), gpay.data()) != 0) { printf("  GPU batch failed\n"); return n; }
  int bad = 0, pass = 0;
  for (int i = 0; i < n; i++) {
    uint64_t cpay; const uint32_t ccrc = cpu_decode(p, vecs[i].data(), &cpay);
    if (ccrc != gcrc[i] || cpay != gpay[i]) {
      if (bad < 3) printf("  MISMATCH %s len=%u AL=%u item %d: cpu crc=%06x pay=%016llx gpu crc=%06x pay=%016llx\n",
                          what, p.len, p.al, i, ccrc, (unsigned long long)cpay, gcrc[i], (unsigned long long)gpay[i]);
      bad++;
    }
    if ((ccrc >> 16) == 0) pass++;
  }
  if (crc_pass) *crc_pass = pass;
  return bad;
}

/* Reconstruct the CPU decoder's u at the information positions from (payload, returned crc):
 * returned = crc24c(ones||A) ^ rxcrc, so rxcrc = returned ^ crc24c(ones||A); B = A<<24 | rxcrc. */
static void cpu_u_from_outputs(int pid, int len, uint64_t Ar, uint32_t ret, uint8_t *u)
{
  int N, K; const uint8_t *info; const uint16_t *il;
  npc_dbg_params(pid, &N, &K, &info, &il);
  uint8_t A64_flip[11] = {0xff, 0xff, 0xff};
  const uint64_t Aprime = Ar << (64 - len);
  for (int i = 0; i < 8; i++) A64_flip[3 + i] = ((const uint8_t *)&Aprime)[7 - i];
  const uint32_t crcA = (crc24c(A64_flip, 24 + len) >> 8) & 0xffffff;
  const uint64_t B = (Ar << 24) | ((ret ^ crcA) & 0xffffff);
  memset(u, 0, N);
  int k = 0;
  for (int n = 0; n < N; n++)
    if (info[n] == 1) { const int t = K - 1 - il[k]; u[n] = (B >> t) & 1; k++; }
}

static int debug_one(void)
{
  npc_gpu_dbg_device_params(NULL);
  const pset p = {39, 1}; const int E = E_of(p); const int pid = npc_register(p.len, p.al);
  { int16_t z[NPC_MAX_E] = {0}; uint64_t o; cpu_decode(p, z, &o); }
  int N, K; const uint8_t *info; const uint16_t *il; npc_dbg_params(pid, &N, &K, &info, &il);
  for (int trial = 0; trial < 5; trial++) {
    std::vector<int16_t> v(E); for (int i = 0; i < E; i++) v[i] = (int16_t)(rnd() & 0xffff);
    npc_item_t it = {pid, v.data()}; uint32_t gcrc; uint64_t gpay;
    npc_decode_batch(&it, 1, &gcrc, &gpay);
    uint64_t cpay; const uint32_t ccrc = cpu_decode(p, v.data(), &cpay);
    if (ccrc == gcrc && cpay == gpay) { printf("trial %d: match\n", trial); continue; }
    uint8_t ucpu[NPC_MAX_N]; cpu_u_from_outputs(pid, p.len, cpay, ccrc, ucpu);
    const uint8_t *ug = npc_dbg_last_u(0);
    { uint32_t rc2; uint64_t rp2; npc_item_t it2 = {pid, v.data()}; npc_ref_decode(&it2, 1, &rc2, &rp2);
      uint8_t uref[NPC_MAX_N]; memcpy(uref, npc_ref_last_u(), NPC_MAX_N);
      npc_decode_batch(&it2, 1, &rc2, &rp2); const uint8_t *ug2 = npc_dbg_last_u(0);
      int fr = -1; for (int nn = 0; nn < N; nn++) if (uref[nn] != ug2[nn]) { fr = nn; break; }
      printf("  HOST(test-TU) sizeof(op)=%zu sizeof(params)=%zu\n", sizeof(npc_op_t), sizeof(npc_dev_params_t));
    int dp[10] = {0}; if (npc_gpu_dbg_fetch(dp) == 0)
      printf("  DEVICE params: N=%d n=%d E=%d rm=%d nops=%d rmp0=%d rmp1=%d DEVICE sizeof(op)=%d sizeof(params)=%d offsetof(ops)=%d\n",
             dp[0],dp[1],dp[2],dp[3],dp[4],dp[5],dp[6],dp[7],dp[8],dp[9]);
    { static int16_t ga[10 * NPC_MAX_N]; const int16_t *ra = npc_ref_last_alpha();
      if (npc_gpu_dbg_alpha(ga) == 0 && ra) {
        for (int L = 0; L <= 7; L++) { int fd = -1;
          for (int i = 0; i < N; i++) if (ga[L * N + i] != ra[L * N + i]) { fd = i; break; }
          if (fd >= 0) { printf("  alpha row L=%d first differs at col %d: ref=%d gpu=%d\n", L, fd, ra[L*N+fd], ga[L*N+fd]); }
        } } }
    { static int16_t gt[4 * NPC_MAX_OPS]; const int16_t *rt = npc_ref_last_trace();
      if (npc_gpu_dbg_trace(gt) == 0 && rt) {
        int code, level, fli; const int nops = npc_ref_op(pid, 0, &code, &level, &fli);
        for (int k = 0; k < nops; k++)
          if (gt[4*k] != rt[4*k] || gt[4*k+1] != rt[4*k+1]) {
            npc_ref_op(pid, k, &code, &level, &fli);
            printf("  FIRST DIVERGING OP: k=%d code=%s level=%d fli=%d sz=%d  ref=(%d,%d) gpu=(%d,%d)\n",
                   k, code==0?"F":(code==1?"G":"B"), level, fli, 1<<(level-1), rt[4*k], rt[4*k+1], gt[4*k], gt[4*k+1]);
            printf("    inputs av[0],av[sz]: ref=(%d,%d) gpu=(%d,%d)\n", rt[4*k+2], rt[4*k+3], gt[4*k+2], gt[4*k+3]);
            break; } } }
    printf("  ref-vs-GPU first differing u index (ALL positions): %d", fr);
      if (fr >= 0) printf("  (ref=%d gpu=%d, info=%d)", uref[fr], ug2[fr], info[fr]);
      printf("\n"); }
    int first = -1, nd = 0;
    for (int n = 0; n < N; n++) if (info[n] && ucpu[n] != ug[n]) { if (first < 0) first = n; nd++; }
    printf("trial %d: MISMATCH N=%d K=%d first differing info position u[%d] (cpu %d gpu %d), %d info bits differ\n", trial, N, K, first, ucpu[first], ug[first], nd);
    int shown = 0;
    for (int n = 0; n < N && shown < 12; n++) if (info[n] && ucpu[n] != ug[n]) { printf("   u[%3d] cpu=%d gpu=%d\n", n, ucpu[n], ug[n]); shown++; }
    return 1;
  }
  return 0;
}

int main(int argc, char **argv)
{
  const bool quick = argc > 1 && !strcmp(argv[1], "quick");
  if (argc > 1 && !strcmp(argv[1], "dbg")) return debug_one();
  int total_bad = 0;
  for (int pi = 0; pi < NP; pi++) {
    const pset &p = P[pi];
    const int E = E_of(p);
    const int pid = npc_register(p.len, p.al);
    if (pid < 0) { printf("register failed len=%u al=%u\n", p.len, p.al); return 1; }
    { int16_t z[NPC_MAX_E] = {0}; uint64_t o; cpu_decode(p, z, &o); } /* warm: steady-state betaInit */

    std::vector<std::vector<int16_t>> v;
    /* (a) full-range random: what 99 % of blind candidates are, and every saturation edge */
    for (int k = 0; k < 300; k++) { v.emplace_back(E); for (int i = 0; i < E; i++) v.back()[i] = (int16_t)(rnd() & 0xffff); }
    for (int k = 0; k < 50; k++) { v.emplace_back(E); for (int i = 0; i < E; i++) { const uint32_t r = rnd() % 4; v.back()[i] = r == 0 ? -32768 : r == 1 ? 32767 : r == 2 ? 0 : (int16_t)(rnd() & 0xffff); } }
    int bad = compare("random", p, pid, v, NULL);
    { /* split kernel bugs from formula bugs: same op list, same formulas, plain sequential C */
      std::vector<npc_item_t> it(v.size()); std::vector<uint32_t> rc(v.size()), gc(v.size());
      std::vector<uint64_t> rp(v.size()), gp(v.size());
      for (size_t i = 0; i < v.size(); i++) it[i] = (npc_item_t){pid, v[i].data()};
      npc_ref_decode(it.data(), (int)v.size(), rc.data(), rp.data());
      npc_decode_batch(it.data(), (int)v.size(), gc.data(), gp.data());
      int ref_vs_oai = 0, ref_vs_gpu = 0;
      for (size_t i = 0; i < v.size(); i++) {
        uint64_t cp; const uint32_t cc = cpu_decode(p, v[i].data(), &cp);
        if (cc != rc[i] || cp != rp[i]) ref_vs_oai++;
        if (rc[i] != gc[i] || rp[i] != gp[i]) ref_vs_gpu++;
      }
      printf("  split: ref-vs-OAI %d, ref-vs-GPU %d (of %zu)\n", ref_vs_oai, ref_vs_gpu, v.size());
    }
    /* (b) real encoder, realistic scale, three SNRs: mostly-pass / mixed / mostly-fail */
    const double amps[3] = {600, 300, 120}, sig[3] = {150, 220, 260};
    int passes[3];
    for (int s = 0; s < 3; s++) {
      std::vector<std::vector<int16_t>> w;
      for (int k = 0; k < 200; k++) { w.emplace_back(E); encode(p, ((uint64_t)rnd() << 32 | rnd()) & ((p.len >= 64) ? ~0ull : ((1ull << p.len) - 1)), 0x4601, amps[s], sig[s], w.back().data()); }
      bad += compare("encoded", p, pid, w, &passes[s]);
    }
    printf("len=%2u AL=%u E=%3d: %d vectors, mismatches=%d, encoded CRC-pass at 3 SNRs = %d/%d/%d of 200\n",
           p.len, p.al, E, (int)v.size() + 600, bad, passes[0], passes[1], passes[2]);
    total_bad += bad;
  }
  printf("BIT-EXACTNESS: %s (%d mismatches)\n", total_bad ? "FAIL" : "PASS", total_bad);

  /* ---- shared-vector path == per-item path (the dlsweep batch shape: every length x every vector) ---- */
  {
    const uint8_t als[4] = {1, 2, 4, 8};
    std::vector<std::vector<int16_t>> vv(8, std::vector<int16_t>(NPC_MAX_E));
    std::vector<int16_t> packed(8 * 864);
    for (int v = 0; v < 8; v++)
      for (int i = 0; i < 864; i++) packed[v * 864 + i] = vv[v][i] = (int16_t)((rnd() % 4000) - 2000);
    std::vector<npc_item_t> items; std::vector<int> pid, vidx;
    for (int len = 30; len <= 63; len++)
      for (int v = 0; v < 8; v++) {
        const int id = npc_register((uint16_t)len, als[v % 4]);
        if (id < 0) continue;
        items.push_back({id, vv[v].data()}); pid.push_back(id); vidx.push_back(v);
      }
    const int n = (int)items.size();
    std::vector<uint32_t> c1(n), c2(n); std::vector<uint64_t> p1(n), p2(n);
    const int r1 = npc_decode_batch(items.data(), n, c1.data(), p1.data());
    const int r2 = npc_decode_batch_vec(packed.data(), 864, 8, vidx.data(), pid.data(), n, c2.data(), p2.data());
    int diff = (r1 || r2) ? n : 0;
    for (int i = 0; i < n && !r1 && !r2; i++) diff += (c1[i] != c2[i] || p1[i] != p2[i]);
    printf("VEC-PATH: %s (%d of %d items differ from the per-item path)\n", diff ? "FAIL" : "PASS", diff, n);
    total_bad += diff;
  }
  if (total_bad || quick) return total_bad ? 1 : 0;

  /* ---- throughput ---------------------------------------------------------------------- */
  const pset bp = {47, 2}; const int bpid = npc_register(bp.len, bp.al); const int E = E_of(bp);
  std::vector<std::vector<int16_t>> pool(1024);
  for (auto &x : pool) { x.resize(E); for (int i = 0; i < E; i++) x[i] = (int16_t)((rnd() % 4000) - 2000); }
  double cpu_us_per_decode = 0.0;
  { /* CPU reference: one core, same vectors */
    const int reps = 2000; double t0 = now_us(); uint64_t o;
    for (int r = 0; r < reps; r++) cpu_decode(bp, pool[r % 1024].data(), &o);
    cpu_us_per_decode = (now_us() - t0) / reps;
    printf("CPU polar_decoder_int16 len=%u AL=%u: %.1f us/decode (one core)\n", bp.len, bp.al, cpu_us_per_decode);
  }
  const int sizes[] = {1, 8, 32, 128, 512, 1024, 2048, 4096};
  for (int si = 0; si < (int)(sizeof(sizes) / sizeof(sizes[0])); si++) {
    const int n = sizes[si];
    std::vector<npc_item_t> items(n); std::vector<uint32_t> c(n); std::vector<uint64_t> pay(n);
    for (int i = 0; i < n; i++) items[i] = (npc_item_t){bpid, pool[i % 1024].data()};
    npc_decode_batch(items.data(), n, c.data(), pay.data()); /* warm */
    const int reps = n >= 2048 ? 10 : (n >= 256 ? 20 : 200);
    double t0 = now_us(); npc_timing_t acc = {0, 0, 0, 0};
    for (int r = 0; r < reps; r++) { npc_decode_batch(items.data(), n, c.data(), pay.data()); npc_timing_t t = npc_last_timing(); acc.h2d += t.h2d; acc.kernel += t.kernel; acc.d2h += t.d2h; acc.host += t.host; }
    const double per = (now_us() - t0) / reps;
    printf("GPU batch %4d: %8.1f us/batch = %6.2f us/decode  [h2d %.1f kernel %.1f d2h %.1f host %.1f]  -> %.0f decodes/s\n",
           n, per, per / n, acc.h2d / reps, acc.kernel / reps, acc.d2h / reps, acc.host / reps, 1e6 * n / per);
  }
  { /* one occasion: K=8 lanes x 13 AL2 candidates x 5 DCI lengths */
    const int lens[5] = {44, 47, 49, 58, 39}; std::vector<int> pids;
    for (int l = 0; l < 5; l++) pids.push_back(npc_register(lens[l], 2));
    const int n = 8 * 13 * 5;
    std::vector<npc_item_t> items(n); std::vector<uint32_t> c(n); std::vector<uint64_t> pay(n);
    for (int i = 0; i < n; i++) items[i] = (npc_item_t){pids[i % 5], pool[i % 1024].data()};
    npc_decode_batch(items.data(), n, c.data(), pay.data());
    double t0 = now_us(); for (int r = 0; r < 20; r++) npc_decode_batch(items.data(), n, c.data(), pay.data());
    /* Use the CPU rate THIS RUN measured, never a constant. A hardcoded 130.2 us/decode used to
     * sit here while the CPU reference a few lines above measured 1.9 us/decode on the same box --
     * a 68x overstatement that made the GPU look ~200x faster than one core when the real, measured
     * ratio is ~4x. That fabricated ratio escaped this file and was quoted as a "271x speedup"
     * justification for GPU work elsewhere. Never print a speedup against a number you did not
     * measure in the same run. */
    const double gpu_us = (now_us() - t0) / 20;
    printf("ONE OCCASION (8 lanes x 13 AL2 x 5 lengths = %d decodes): %.0f us on GPU vs %.0f us on one "
           "CPU core (measured %.1f us/decode) -> %.1fx\n",
           n, gpu_us, n * cpu_us_per_decode, cpu_us_per_decode, (n * cpu_us_per_decode) / gpu_us);
  }
  return 0;
}
