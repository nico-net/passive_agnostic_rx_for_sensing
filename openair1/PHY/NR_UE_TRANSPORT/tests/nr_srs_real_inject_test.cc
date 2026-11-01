/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Whole live SRS chain on REAL captured uplink slots with a synthetic SRS added: nr_srs_passive_capture (the call nr-ue.c makes every slot) -> queue -> worker -> time-domain gate ->
 * lease -> FFT scan -> detect -> tracker -> nr_srs_export. The capture is read-only (mmap); the SRS (TS 38.211 6.4.1.4 sequence from the production generate_srs_nr) is added into the sample
 * ring of the receiver at a chosen per-RE SNR measured against the capture's own noise in the same bins. No grants are posted: the real PUCCH / PUSCH of the capture are unexplained traffic.
 * usage: test_nr_srs_real_inject --capture <dir> --slot0 <sample of the first slot of a 10-slot TDD pattern, from tools/srs/srs_physics_check.py> [--seconds 0.8]
 * Without --capture the cases are skipped. */
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "common/utils/nr_cell.h"
extern "C" {
#include "PHY/defs_nr_UE.h"
#include "PHY/TOOLS/tools_defs.h"
#include "nr_srs_passive.h"
#include "nr_srs_blind.h"
#include "nr_sense_api.h"
#include "PHY/NR_UE_ISAC/nr_isac.h"
void srs_test_init(void);
bool srs_test_generate(const nr_srs_grid_t *, const nr_srs_candidate_t *, int16_t *);
int __wrap_nr_isac_source_enabled(int source) { return source == NR_ISAC_SRC_SRS; }
bool nr_pdcch_blind_monitor_rnti_confirmed(uint64_t, uint16_t) { return true; }
std::atomic<int> g_stall_us{0};
int __wrap_nr_pool_site_float_acquire(int, int, uint64_t)
{
  if (g_stall_us.load()) usleep(g_stall_us.load());
  return -1;
}
/* numerology 1, normal CP at 122.88 Msps: 4096+288 samples per symbol, 352 for the first symbol of each half subframe */
static int cpl(int l) { return (l % 7 == 0) ? 352 : 288; }
uint32_t get_samples_symbol_duration(const NR_DL_FRAME_PARMS *fp, int, int l, int) { return fp->ofdm_symbol_size + cpl(l); }
uint32_t get_samples_symbol_timestamp(const NR_DL_FRAME_PARMS *fp, int, int l)
{
  uint32_t t = 0;
  for (int i = 0; i < l; i++) t += fp->ofdm_symbol_size + cpl(i);
  return t;
}
uint32_t get_samples_slot_timestamp(const NR_DL_FRAME_PARMS *, unsigned int slot) { return slot * 61440; }
}

namespace {
std::string g_capture;
long g_slot0 = -1;
double g_seconds = 0.8;
std::vector<double> g_snr; int g_cfg = -1;
bool g_white = false; /* --white: replace the capture by white Gaussian noise of the same power (separates the capture's colour/structure from the detector) */
struct { uint64_t x = 88172645463325252ull; double next() { double u = 0; for (int i = 0; i < 12; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; u += (x >> 11) * (1.0 / 9007199254740992.0); } return u - 6; } } g_rng;
constexpr int N = 4096, NRB = 217, FCO = 4096 - 217 * 6, NANT = 4, SLOT = 61440, MARGIN = 400;
const nr_srs_grid_t G{N, FCO, NRB, NANT, 14, 20};

struct Capture {
  int16_t *rx[NANT] = {};
  size_t samples = 0;
  bool open(const std::string &dir)
  {
    for (int a = 0; a < NANT; a++) {
      std::string p = dir + "/rx" + std::to_string(a) + ".sc16";
      int fd = ::open(p.c_str(), O_RDONLY);
      if (fd < 0) return false;
      struct stat st;
      fstat(fd, &st);
      samples = st.st_size / 4;
      void *m = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
      ::close(fd);
      if (m == MAP_FAILED) return false;
      rx[a] = (int16_t *)m;
    }
    return true;
  }
};

struct Bed {
  PHY_VARS_NR_UE ue;
  std::vector<c16_t *> rx;
  std::vector<c16_t> buf[NANT];
  std::vector<fapi_nr_max_num_of_symbol_per_slot_t> sym[10];
  std::vector<fapi_nr_max_tdd_periodicity_t> per;
  Bed()
  {
    memset(&ue, 0, sizeof(ue));
    NR_DL_FRAME_PARMS *fp = &ue.frame_parms;
    fp->ofdm_symbol_size = N; fp->first_carrier_offset = FCO; fp->N_RB_UL = fp->N_RB_DL = NRB;
    fp->nb_antennas_rx = NANT; fp->symbols_per_slot = 14; fp->slots_per_frame = 20; fp->slots_per_subframe = 2;
    fp->samples_per_subframe = N * 15 * 2; fp->samples_per_frame = fp->samples_per_subframe * 10;
    fp->numerology_index = 1; fp->frame_type = TDD; fp->ofdm_offset_divisor = 8; fp->nb_prefix_samples = 288;
    for (int i = 0; i < 224; i++) fp->symbol_rotation[link_type_ul][i] = (c16_t){32767, 0};
    for (int k = 0; k < N; k++) fp->timeshift_symbol_rotation[k] = (c16_t){32767, 0};
    per.resize(10);
    for (int s = 0; s < 10; s++) { /* DDDDDDDSUU: slot 7 special (6 DL, 4 guard, 4 UL symbols), 8 and 9 uplink */
      sym[s].resize(14);
      for (int l = 0; l < 14; l++) sym[s][l].slot_config = (s >= 8) || (s == 7 && l >= 10);
      per[s].max_num_of_symbol_per_slot_list = sym[s].data();
    }
    ue.nrUE_config.tdd_table.tdd_period_in_slots = 10;
    ue.nrUE_config.tdd_table.max_tdd_periodicity_list = per.data();
    rx.resize(NANT);
    for (int a = 0; a < NANT; a++) { buf[a].assign(fp->samples_per_frame, c16_t{0, 0}); rx[a] = buf[a].data(); }
    ue.common_vars.rxdata = rx.data();
  }
};

struct Cfg { const char *name; int comb, comb_off, start_symbol, n_symbols, start_rb, nb_rb, group, cs, period, slot; };
struct Out { unsigned records, confirmed; double snr_db; nr_ptrack_counters_t pc; uint64_t dropped, gate_unexpl, gate_pusch, budget; double cpu_ms; };

/* noise (mean |FFT bin|^2 per antenna) of the capture in bins [k0,k0+n) measured on `count` uplink symbols of slots 8/9 */
double capture_noise(const Capture &cap, int k0, int n_bins, int count)
{
  double acc = 0; int used = 0;
  c16_t body[N] __attribute__((aligned(32))), spec[N] __attribute__((aligned(32)));
  for (int p = 0; p < count; p++) {
    long slot = g_slot0 + (long)(p * 10 + 8) * SLOT;
    int l = 2 + (p % 4);
    long s = slot + l * 4384 + 1000;
    if ((size_t)(s + N) > cap.samples) break;
    for (int a = 0; a < NANT; a++) {
      memcpy(body, cap.rx[a] + 2 * s, sizeof(body));
      dft(get_dft(N), (int16_t *)body, (int16_t *)spec, 1);
      for (int k = 0; k < n_bins; k++) { const c16_t &v = spec[(k0 + k) % N]; acc += (double)v.r * v.r + (double)v.i * v.i; used++; }
    }
  }
  return used ? acc / used : 1;
}

Out run(const Capture &cap, const Cfg &c, double snr_db, int stall_us)
{
  Bed bed;
  nr_cell_bind(0);
  nr_srs_passive_cell_prealloc(0);
  char path[] = "/tmp/srs-real-XXXXXX";
  int fd = mkstemp(path); close(fd);
  std::string cfr = std::string(path) + ".cfr";
  nr_sense_cfg_t cfg{path, cfr.c_str(), nullptr, 64, 217 * 12, 256, 4};
  nr_sense_open(&cfg);
  nr_srs_passive_start(&bed.ue);
  nr_srs_candidate_t cand{};
  cand.comb = c.comb; cand.comb_offset = c.comb_off; cand.start_symbol = c.start_symbol; cand.n_symbols = c.n_symbols;
  cand.start_rb = c.start_rb; cand.nb_rb = c.nb_rb; cand.group = c.group; cand.cs = c.cs;
  std::vector<int16_t> grid(4 * 14 * N * 2 + 64, 0);
  EXPECT_TRUE(srs_test_generate(&G, &cand, grid.data()));
  /* time-domain symbols of the SRS at unit gain, and the per-RE power they give in the capture's own FFT convention */
  const int k0 = (FCO + c.start_rb * 12) % N, nbins = c.nb_rb * 12;
  const double pn = capture_noise(cap, k0, nbins, 200);
  std::vector<c16_t> tsym((size_t)14 * N);
  double ps = 0; int ns = 0;
  for (int l = c.start_symbol; l < c.start_symbol + c.n_symbols; l++) {
    c16_t spec[N] __attribute__((aligned(32))), body[N] __attribute__((aligned(32))), back[N] __attribute__((aligned(32)));
    memcpy(spec, grid.data() + 2 * l * N, sizeof(spec));
    idft(get_idft(N), (int16_t *)spec, (int16_t *)body, 1);
    dft(get_dft(N), (int16_t *)body, (int16_t *)back, 1);
    for (int k = 0; k < N; k++) if (spec[k].r || spec[k].i) { ps += (double)back[k].r * back[k].r + (double)back[k].i * back[k].i; ns++; }
    memcpy(&tsym[(size_t)l * N], body, sizeof(body));
  }
  ps /= ns ? ns : 1;
  const double gain = std::sqrt(pn / ps * std::pow(10.0, snr_db / 10.0));
  Out o{}; o.snr_db = snr_db;
  const NR_DL_FRAME_PARMS *fp = &bed.ue.frame_parms;
  const int adv = 25600 * fp->samples_per_subframe / (4096 * 480);
  const long frame = fp->samples_per_frame;
  uint64_t b_dr, d1, d2, d3, d4, us0;
  nr_srs_passive_metrics(&us0, &b_dr, &d1, &d2, &d3, &d4);
  g_stall_us = stall_us;
  const long total = (long)(g_seconds * 2000);
  auto t0 = std::chrono::steady_clock::now();
  std::vector<c16_t> added;
  for (long s = 1; s < total; s++) {
    const long prev = s - 1;
    /* ring <- the capture, slot `prev` with one slot of margin each side (the ring is one frame, indexed like the frame) */
    for (long q = prev - 1; q <= prev + 1; q++) {
      if (q < 0) continue;
      long file0 = g_slot0 + q * SLOT;
      if ((size_t)(file0 + SLOT) > cap.samples) goto done;
      for (int a = 0; a < NANT; a++)
        for (int i = 0; i < SLOT; i++) {
          long ri = ((q % 20) * SLOT + i) % frame;
          bed.buf[a][ri] = g_white ? c16_t{(int16_t)lrint(g_rng.next() * 18), (int16_t)lrint(g_rng.next() * 18)}
                                    : c16_t{cap.rx[a][2 * (file0 + i)], cap.rx[a][2 * (file0 + i) + 1]};
        }
    }
    {
      const bool occ = (prev % c.period) == c.slot;
      if (occ) /* add the SRS where the receiver's FFT window of each SRS symbol starts (aligned with the body) */
        for (int l = c.start_symbol; l < c.start_symbol + c.n_symbols; l++) {
          long w = (prev % 20) * SLOT + get_samples_symbol_timestamp(fp, 0, l) + cpl(l) - fp->nb_prefix_samples / fp->ofdm_offset_divisor - adv;
          for (int a = 0; a < NANT; a++)
            for (int i = -cpl(l); i < N; i++) {
              c16_t v = tsym[(size_t)l * N + (i + N) % N];
              c16_t &d = bed.buf[a][((w + i) % frame + frame) % frame];
              d.r += (int16_t)lrint(gain * v.r); d.i += (int16_t)lrint(gain * v.i);
            }
        }
      nr_srs_passive_capture(&bed.ue, (int)((s / 20) % 1024), (int)(s % 20), s);
    }
    std::this_thread::sleep_until(t0 + std::chrono::microseconds(500 * s));
  }
done:
  g_stall_us = 0;
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  uint64_t audits, dr, pg, bo, bu, rs, us1;
  nr_srs_passive_periodic_metrics(&o.pc, &audits);
  nr_srs_passive_gate_metrics(&o.gate_unexpl, &o.gate_pusch);
  nr_srs_passive_metrics(&us1, &dr, &pg, &bo, &bu, &rs);
  o.dropped = dr - b_dr; o.budget = bu; o.cpu_ms = us1 / 1000.0;
  nr_srs_passive_stop();
  nr_sense_close();
  std::ifstream in(path); std::string line;
  while (std::getline(in, line)) {
    if (line.find("\"source\":\"srs\"") == std::string::npos) continue;
    o.records++;
    if (line.find("\"confirmed\":true") != std::string::npos) o.confirmed++;
  }
  unlink(path); unlink(cfr.c_str());
  return o;
}
}

class RealInject : public testing::Test {
 protected:
  static Capture cap;
  static bool ok;
  static void SetUpTestSuite() { srs_test_init(); ok = !g_capture.empty() && g_slot0 >= 0 && cap.open(g_capture); }
};
Capture RealInject::cap;
bool RealInject::ok = false;

TEST_F(RealInject, SnrSweepAcrossConfigs)
{
  if (!ok) GTEST_SKIP() << "no --capture / --slot0";
  const Cfg cfgs[] = {
    {"comb2 216RB 2sym  p20", 2, 0, 12, 2, 0, 216, 17, 3, 20, 9},
    {"comb4 216RB 2sym  p40", 4, 1, 12, 2, 0, 216, 33, 5, 40, 29},
    {"comb2 96RB 1sym   p20", 2, 1, 13, 1, 20, 96, 9, 0, 20, 19},
    {"comb2 48RB 4sym   p40", 2, 0, 10, 4, 40, 48, 41, 2, 40, 9},
    {"comb4 24RB 2sym   p80", 4, 0, 12, 2, 60, 24, 3, 1, 80, 29},
  };
  const std::vector<double> snrs = g_snr.empty() ? std::vector<double>{-5.0, 0.0, 5.0} : g_snr;
  int ci = -1;
  for (const Cfg &c : cfgs) for (double snr : snrs) {
    if (snr == snrs[0]) ci++;
    if (g_cfg >= 0 && ci != g_cfg) continue;
    Out o = run(cap, c, snr, 0);
    printf("REAL %-24s per-RE SNR %+5.1f dB: records %3u confirmed %3u | hit %lu predicted %lu | gate_unexplained %lu pusch %lu | budget_skip %lu dropped %lu cpu %.0f ms\n", c.name, snr,
           o.records, o.confirmed, (unsigned long)o.pc.hit, (unsigned long)o.pc.predicted, (unsigned long)o.gate_unexpl, (unsigned long)o.gate_pusch, (unsigned long)o.budget, (unsigned long)o.dropped, o.cpu_ms);
    fflush(stdout);
    /* what the gate can see: the SRS must carry enough of the 78 MHz band's power (24 RB comb 4 is 2.8 % of it, below the gate) */
    const bool must = (c.nb_rb >= 96 && snr >= 0) || (c.nb_rb >= 48 && snr >= 5) || (c.nb_rb >= 216 && snr >= 5);
    if (must) { EXPECT_GE(o.records, 10u) << c.name << " " << snr; EXPECT_GE(o.pc.confirmed, 1u) << c.name << " " << snr; EXPECT_EQ(o.dropped, 0u); }
  }
}

int main(int argc, char **argv)
{
  std::vector<char *> rest;
  for (int i = 0; i < argc; i++) {
    if (!strcmp(argv[i], "--capture") && i + 1 < argc) g_capture = argv[++i];
    else if (!strcmp(argv[i], "--slot0") && i + 1 < argc) g_slot0 = atol(argv[++i]);
    else if (!strcmp(argv[i], "--white")) g_white = true;
    else if (!strcmp(argv[i], "--snr") && i + 1 < argc) g_snr.push_back(atof(argv[++i]));
    else if (!strcmp(argv[i], "--cfg") && i + 1 < argc) g_cfg = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) g_seconds = atof(argv[++i]);
    else rest.push_back(argv[i]);
  }
  int n = (int)rest.size();
  testing::InitGoogleTest(&n, rest.data());
  return RUN_ALL_TESTS();
}

/* The capture alone (no SRS): the real uplink traffic of the cell must neither open the gate nor starve the queue. */
TEST_F(RealInject, CaptureWithoutSrsStaysQuiet)
{
  if (!ok) GTEST_SKIP() << "no --capture / --slot0";
  const Cfg c{"none", 2, 0, 12, 2, 0, 216, 17, 3, 20, 9};
  Out o = run(cap, c, -200.0, 0);
  printf("REAL no SRS: records %u gate_unexplained %lu gate_pusch %lu budget_skip %lu dropped %lu cpu %.0f ms\n", o.records, (unsigned long)o.gate_unexpl, (unsigned long)o.gate_pusch,
         (unsigned long)o.budget, (unsigned long)o.dropped, o.cpu_ms);
  EXPECT_EQ(o.records, 0u);
  EXPECT_LE(o.gate_unexpl, 2u);
  EXPECT_EQ(o.dropped, 0u);
  EXPECT_EQ(o.budget, 0u);
}
