/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Synthetic SRS (TS 38.211 6.4.1.4 sequence, comb 2 / 4, several periods) written into the receive-sample ring of a TDD UE and pushed through the PRODUCTION job path
 * (nr_srs_passive_capture -> queue -> worker -> process_srs -> tracker -> nr_srs_export). The float-lease wait of the pool is modelled by a stall in
 * nr_pool_site_float_acquire (--wrap): a busy host holds the HOUSE class for tens of milliseconds behind DECODE / DISC work (SENSING: CORE_POOL long wait class=4). */
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
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
std::atomic<int> g_leases{0};
int __wrap_nr_pool_site_float_acquire(int, int, uint64_t)
{
  g_leases++;
  if (g_stall_us.load()) usleep(g_stall_us.load());
  return -1;
}
/* self-consistent timing: cyclic prefix N/14, symbols back to back */
uint32_t get_samples_symbol_duration(const NR_DL_FRAME_PARMS *fp, int, int, int) { return fp->ofdm_symbol_size + fp->ofdm_symbol_size / 14; }
uint32_t get_samples_symbol_timestamp(const NR_DL_FRAME_PARMS *fp, int, int l) { return l * (fp->ofdm_symbol_size + fp->ofdm_symbol_size / 14); }
uint32_t get_samples_slot_timestamp(const NR_DL_FRAME_PARMS *fp, unsigned int slot) { return slot * 14 * (fp->ofdm_symbol_size + fp->ofdm_symbol_size / 14); }
}

namespace {
constexpr int N = 4096, NRB = 24, FCO = 4096 - 144, NANT = 4, SLOTS = 20, MARGIN = 400;
constexpr int SYM = N + N / 14;
const nr_srs_grid_t G{N, FCO, NRB, NANT, 14, SLOTS};

struct Bed {
  PHY_VARS_NR_UE ue;
  std::vector<c16_t *> rx;
  std::vector<c16_t> rxbuf[NANT];
  std::vector<fapi_nr_max_num_of_symbol_per_slot_t> sym[10];
  std::vector<fapi_nr_max_tdd_periodicity_t> per;
  uint32_t rng = 7;
  Bed()
  {
    memset(&ue, 0, sizeof(ue));
    NR_DL_FRAME_PARMS *fp = &ue.frame_parms;
    fp->ofdm_symbol_size = N;
    fp->first_carrier_offset = FCO;
    fp->N_RB_UL = fp->N_RB_DL = NRB;
    fp->nb_antennas_rx = NANT;
    fp->symbols_per_slot = 14;
    fp->slots_per_frame = SLOTS;
    fp->slots_per_subframe = 2;
    fp->samples_per_subframe = N * 15 * 2;
    fp->samples_per_frame = fp->samples_per_subframe * 10;
    fp->numerology_index = 1;
    fp->frame_type = TDD;
    fp->ofdm_offset_divisor = 8;
    fp->nb_prefix_samples = N / 14;
    for (int i = 0; i < 224; i++) fp->symbol_rotation[link_type_ul][i] = (c16_t){32767, 0};
    for (int k = 0; k < N; k++) fp->timeshift_symbol_rotation[k] = (c16_t){32767, 0};
    /* DDDSU DDDSU: slots 4 and 9 uplink, 3 and 8 special (last 4 symbols uplink) */
    per.resize(10);
    for (int s = 0; s < 10; s++) {
      sym[s].resize(14);
      for (int l = 0; l < 14; l++) {
        int ul = (s % 5 == 4) || (s % 5 == 3 && l >= 10);
        sym[s][l].slot_config = ul ? 1 : 0;
      }
      per[s].max_num_of_symbol_per_slot_list = sym[s].data();
    }
    ue.nrUE_config.tdd_table.tdd_period_in_slots = 10;
    ue.nrUE_config.tdd_table.max_tdd_periodicity_list = per.data();
    rx.resize(NANT);
    for (int a = 0; a < NANT; a++) {
      rxbuf[a].assign(fp->samples_per_frame, c16_t{0, 0});
      for (auto &v : rxbuf[a]) v = {noise(), noise()};
      rx[a] = rxbuf[a].data();
    }
    ue.common_vars.rxdata = rx.data();
  }
  int16_t noise() { rng = rng * 1664525u + 1013904223u; return (int)(rng >> 27) - 16; }
  /* adds (sign +1) or removes (sign -1) the SRS symbols of slot `s` (absolute) in the sample ring, which holds noise otherwise */
  void write_slot(uint64_t s, const nr_srs_candidate_t *srs, const int16_t *grid, int sign)
  {
    const NR_DL_FRAME_PARMS *fp = &ue.frame_parms;
    const int adv = 25600 * fp->samples_per_subframe / (4096 * 480);
    const int64_t base = (int64_t)(s % SLOTS) * 14 * SYM;
    for (int a = 0; a < NANT; a++) {
      if (!srs) continue;
      for (int l = srs->start_symbol; l < srs->start_symbol + srs->n_symbols; l++) {
        c16_t spectrum[N] __attribute__((aligned(32))), body[N] __attribute__((aligned(32)));
        memcpy(spectrum, grid + 2 * l * N, sizeof(spectrum));
        for (int k = 0; k < N; k++) { spectrum[k].r = spectrum[k].r / 2; spectrum[k].i = spectrum[k].i / 2; }
        idft(get_idft(N), (int16_t *)spectrum, (int16_t *)body, 1);
        /* the receiver's FFT window of symbol l starts here (nr_srs_passive_capture); the body is aligned with it, the cyclic prefix is the body tail */
        int64_t w = base + (int64_t)l * SYM + N / 14 - fp->nb_prefix_samples / fp->ofdm_offset_divisor - adv;
        for (int i = -N / 14; i < N; i++) {
          c16_t v = body[(i + N) % N];
          c16_t &d = rxbuf[a][((w + i) % fp->samples_per_frame + fp->samples_per_frame) % fp->samples_per_frame];
          d.r += sign * v.r;
          d.i += sign * v.i;
        }
      }
    }
  }
};

struct Result {
  unsigned records;
  unsigned confirmed_records;
  nr_ptrack_counters_t pc;
  uint64_t gate_unexplained, gate_pusch;
  uint64_t dropped, backoff, leases, budget, pregate, resets, us;
  double cpu_us_per_job;
};

/* Runs `periods` SRS periods at the slot rate (0.5 ms) and returns what reached the record file. */
/* dmrs: every UL slot also carries a comb-2 PUSCH DM-RS (symbol 9, other RBs) of a grant the receiver knows (nr_srs_passive_grant), as a PUSCH of another UE would;
 * srs_on false = the DM-RS is the only comb signal. */
Result run(int comb, int comb_off, int period, int slot_off, int periods, int stall_us, bool dmrs = false, bool srs_on = true)
{
  Bed bed;
  nr_cell_bind(0);
  nr_srs_passive_cell_prealloc(0);
  char path[] = "/tmp/srs-prod-XXXXXX";
  int fd = mkstemp(path);
  close(fd);
  std::string cfr = std::string(path) + ".cfr";
  nr_sense_cfg_t cfg{path, cfr.c_str(), nullptr, 64, 288 * 4, 256, 4};
  EXPECT_EQ(nr_sense_open(&cfg), 0);
  nr_srs_passive_start(&bed.ue);
  EXPECT_TRUE(nr_srs_passive_enabled());
  nr_srs_candidate_t c{};
  c.comb = comb; c.comb_offset = comb_off; c.start_symbol = 12; c.n_symbols = 2; c.start_rb = 4; c.nb_rb = 8; c.group = 17; c.cs = 3;
  std::vector<int16_t> grid(4 * 14 * N * 2 + 64, 0);
  EXPECT_TRUE(srs_test_generate(&G, &c, grid.data()));
  nr_srs_candidate_t dm = c;
  dm.comb = 2; dm.comb_offset = 1; dm.start_symbol = 9; dm.n_symbols = 1; dm.start_rb = 12; dm.nb_rb = 8; dm.group = 5; dm.cs = 0;
  std::vector<int16_t> dgrid(4 * 14 * N * 2 + 64, 0);
  EXPECT_TRUE(srs_test_generate(&G, &dm, dgrid.data()));
  uint64_t b_dropped, b_backoff, d1, d2, d3;
  uint64_t us0, us1;
  nr_srs_passive_metrics(&us0, &b_dropped, &d1, &b_backoff, &d2, &d3);
  g_leases = 0;
  g_stall_us = stall_us;
  const uint64_t total = (uint64_t)period * periods;
  auto t0 = std::chrono::steady_clock::now();
  for (uint64_t s = 1; s < total; s++) {
    /* the capture at slot s serves slot s-1 */
    bool occ = ((s - 1) % period) == (uint64_t)slot_off;
    const bool ul = ((s - 1) % 5) == 4;
    const bool srs_here = occ && srs_on;
    const bool dm_here = dmrs && ul;
    if (dm_here) { nr_srs_passive_grant(s - 1, 1u << 9, 12, 8); bed.write_slot(s - 1, &dm, dgrid.data(), 1); }
    bed.write_slot(s - 1, srs_here ? &c : nullptr, grid.data(), 1);
    nr_srs_passive_capture(&bed.ue, (int)((s / SLOTS) % 1024), (int)(s % SLOTS), s);
    bed.write_slot(s - 1, srs_here ? &c : nullptr, grid.data(), -1);
    if (dm_here) bed.write_slot(s - 1, &dm, dgrid.data(), -1);
    std::this_thread::sleep_until(t0 + std::chrono::microseconds(500 * s));
  }
  g_stall_us = 0;
  uint64_t dr_loop, x1, x2, x3, x4, x5; /* drops while slots were being captured (after the last capture nothing drains the event ring any more) */
  nr_srs_passive_metrics(&x1, &dr_loop, &x2, &x3, &x4, &x5);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  Result r{};
  uint64_t audits;
  nr_srs_passive_periodic_metrics(&r.pc, &audits);
  uint64_t dr;
  nr_srs_passive_gate_metrics(&r.gate_unexplained, &r.gate_pusch);
  nr_srs_passive_metrics(&us1, &dr, &r.pregate, &r.backoff, &r.budget, &r.resets);
  r.us = us1;
  (void)dr;
  r.dropped = dr_loop - b_dropped;
  r.backoff -= b_backoff;
  r.leases = g_leases;
  r.cpu_us_per_job = (double)us1 / (r.leases ? r.leases : 1);
  nr_srs_passive_stop();
  nr_sense_close();
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.find("\"source\":\"srs\"") == std::string::npos) continue;
    r.records++;
    if (line.find("\"confirmed\":true") != std::string::npos) r.confirmed_records++;
  }
  unlink(path);
  unlink(cfr.c_str());
  return r;
}
}

class SrsProduction : public testing::Test {
 protected:
  static void SetUpTestSuite() { srs_test_init(); }
};

/* comb 2, period 20 slots, UL slot 9: exported and confirmed although the pool makes every lease wait 10 ms */
TEST_F(SrsProduction, Comb2Period20ExportedAndConfirmedUnderLeaseStall)
{
  Result r = run(2, 0, 20, 9, 150, 10000);
  printf("records=%u confirmed=%u hit=%lu predicted=%lu dropped=%lu backoff_skip=%lu leases=%lu\n", r.records, r.confirmed_records,
         (unsigned long)r.pc.hit, (unsigned long)r.pc.predicted, (unsigned long)r.dropped, (unsigned long)r.backoff, (unsigned long)r.leases);
  EXPECT_GE(r.records, 20u);
  EXPECT_GE(r.pc.confirmed, 1u);
  EXPECT_GE(r.confirmed_records, 10u);
}

/* comb 4, offset 1, period 40 slots, slot 29 (UL slot 9 of the second half-frame) */
TEST_F(SrsProduction, Comb4Period40ExportedAndConfirmedUnderLeaseStall)
{
  Result r = run(4, 1, 40, 29, 75, 10000);
  printf("records=%u confirmed=%u hit=%lu predicted=%lu dropped=%lu backoff_skip=%lu leases=%lu\n", r.records, r.confirmed_records,
         (unsigned long)r.pc.hit, (unsigned long)r.pc.predicted, (unsigned long)r.dropped, (unsigned long)r.backoff, (unsigned long)r.leases);
  EXPECT_GE(r.records, 10u);
  EXPECT_GE(r.pc.confirmed, 1u);
}

/* Without any lease stall the same stream is detected too (the detector itself works). */
TEST_F(SrsProduction, Comb2Period20NoStall)
{
  Result r = run(2, 0, 20, 9, 150, 0);
  printf("us=%lu pregate=%lu backoff=%lu budget=%lu resets=%lu audits\n", (unsigned long)r.us, (unsigned long)r.pregate, (unsigned long)r.backoff, (unsigned long)r.budget, (unsigned long)r.resets);
  printf("records=%u confirmed=%u hit=%lu dropped=%lu leases=%lu\n", r.records, r.confirmed_records, (unsigned long)r.pc.hit, (unsigned long)r.dropped, (unsigned long)r.leases);
  EXPECT_GE(r.records, 20u);
  EXPECT_GE(r.pc.confirmed, 1u);
}

/* An idle cell (noise only) under a lease stall: skipped slots are not counted as dropped jobs and take no lease. */
TEST_F(SrsProduction, IdleCellDropsNothingAndLeasesRarely)
{
  Result r = run(2, 0, 20, 99999, 150, 10000);
  printf("idle: dropped=%lu backoff_skip=%lu leases=%lu worker_cpu_us=%lu eligible_slots=%d\n", (unsigned long)r.dropped, (unsigned long)r.backoff, (unsigned long)r.leases, (unsigned long)r.us, 150 * 20 * 4 / 10);
  EXPECT_EQ(r.records, 0u);
  EXPECT_LE(r.dropped, 20u);
  EXPECT_EQ(r.leases, 0u) << "noise needs no FFT scan and no pool lease";
}

TEST_F(SrsProduction, Comb4Period40NoStall)
{
  Result r = run(4, 1, 40, 29, 75, 0);
  printf("c4 nostall records=%u hit=%lu dropped=%lu leases=%lu pregate=%lu backoff=%lu\n", r.records, (unsigned long)r.pc.hit, (unsigned long)r.dropped, (unsigned long)r.leases, (unsigned long)r.pregate, (unsigned long)r.backoff);
}

/* PUSCH DM-RS (comb 2, a known grant) is the only comb signal on every UL slot: the gate must not start scans; the opens are counted as PUSCH-explained. */
TEST_F(SrsProduction, PuschDmrsOnlySlotDoesNotOpenTheGate)
{
  Result r = run(2, 0, 20, 9, 60, 0, true, false);
  printf("dmrs only: unexplained=%lu pusch=%lu leases=%lu records=%u\n", (unsigned long)r.gate_unexplained, (unsigned long)r.gate_pusch, (unsigned long)r.leases, r.records);
  /* a grant event is posted with a try-lock (a miss is a counted drop), so a few slots can reach the gate without their grant */
  EXPECT_LE(r.gate_unexplained, 3u);
  EXPECT_GT(r.gate_pusch, 100u);
  EXPECT_LE(r.leases, 3u);
  EXPECT_LE(r.records, 3u);
}

/* SRS in symbols 12-13 with the PUSCH DM-RS of another UE in symbol 9 of the same slots: the SRS is still found and confirmed. */
TEST_F(SrsProduction, SrsWithConcurrentPuschDmrsStillOpensAndConfirms)
{
  Result r = run(2, 0, 20, 9, 150, 0, true, true);
  printf("srs+dmrs: unexplained=%lu pusch=%lu records=%u confirmed=%u\n", (unsigned long)r.gate_unexplained, (unsigned long)r.gate_pusch, r.records, r.confirmed_records);
  EXPECT_GT(r.gate_unexplained, 0u);
  EXPECT_GT(r.gate_pusch, 0u);
  EXPECT_GE(r.records, 20u);
  EXPECT_GE(r.pc.confirmed, 1u);
}

/* Live drop path: other threads (PDCCH / PUSCH monitors) post grant and PUSCH-CFR events all the time while the slot thread captures. The capture used to hold the SRS lock during its
 * 470 KB sample copy, so every event posted meanwhile failed its try-lock and counted as a dropped job; events were also only drained when a job ran. */
TEST_F(SrsProduction, EventTrafficDoesNotCountAsDroppedJobs)
{
  std::atomic<bool> stop{false};
  std::atomic<long> posted{0};
  auto poster = [&]() {
    nr_cell_bind(0);
    float cfr[2 * 48] = {0};
    uint32_t k[48];
    for (int i = 0; i < 48; i++) k[i] = i;
    uint64_t slot = 0;
    while (!stop) {
      nr_srs_passive_grant(slot, 1u << 9, 12, 8);
      nr_srs_passive_pusch(100 + (slot & 7), slot, 0, cfr, k, 48);
      slot++;
      posted += 2;
      std::this_thread::sleep_for(std::chrono::microseconds(500)); // ~4k events/s in all: the busy captures post ~1-2k/s
    }
  };
  std::thread t1(poster), t2(poster);
  Result r = run(2, 0, 20, 99999, 150, 0);
  stop = true;
  t1.join();
  t2.join();
  printf("events posted %ld, dropped %lu\n", (long)posted, (unsigned long)r.dropped);
  EXPECT_LE(r.dropped, (uint64_t)(posted / 100)) << "event posts must not fail on the lock the capture copy used to hold";
}
