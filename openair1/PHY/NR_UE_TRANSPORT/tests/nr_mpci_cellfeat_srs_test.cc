/* mpci CELLFEAT: the passive SRS searcher is per cell (queue, event ring, worker, counters). Cell 1's events, drops and stop never show on cell 0. */
#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include "common/utils/nr_cell.h"
extern "C" {
#include "PHY/defs_nr_UE.h"
#include "nr_srs_passive.h"
#include "nr_sense_api.h"
#include "PHY/NR_UE_ISAC/nr_isac.h"
void srs_test_init(void);
/* the SRS source is "enabled" and a session "open" for every cell of this test */
bool __wrap_nr_sense_enabled(void) { return true; }
int __wrap_nr_isac_source_enabled(int source) { return source == NR_ISAC_SRC_SRS; }
/* the RNTI bootstrap table of the receiver is not linked here: every RNTI counts as confirmed */
bool nr_pdcch_blind_monitor_rnti_confirmed(uint64_t, uint16_t) { return true; }
/* timing helpers of PHY/INIT/nr_parms.c: no capture reaches them (the captures of this test are refused before the symbol copy) */
uint32_t get_samples_symbol_duration(const NR_DL_FRAME_PARMS *, int, int, int) { return 0; }
uint32_t get_samples_symbol_timestamp(const NR_DL_FRAME_PARMS *, int, int) { return 0; }
uint32_t get_samples_slot_timestamp(const NR_DL_FRAME_PARMS *, unsigned int) { return 0; }
}

namespace {
void make_ue(PHY_VARS_NR_UE *ue, int fft, int first_carrier, int n_rb, int n_ant)
{
  memset(ue, 0, sizeof(*ue));
  NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  fp->ofdm_symbol_size = fft;
  fp->first_carrier_offset = first_carrier;
  fp->N_RB_UL = n_rb;
  fp->N_RB_DL = n_rb;
  fp->nb_antennas_rx = n_ant;
  fp->symbols_per_slot = 14;
  fp->slots_per_frame = 20;
  fp->slots_per_subframe = 2;
  fp->samples_per_subframe = fft * 15 * 2;
  fp->samples_per_frame = fp->samples_per_subframe * 10;
  fp->numerology_index = 1;
  fp->frame_type = TDD;
  fp->ofdm_offset_divisor = 8;
  fp->nb_prefix_samples = fft / 14;
}
struct Dropped {
  uint64_t us, dropped, pregate, backoff, budget, resets;
};
Dropped metrics()
{
  Dropped m{};
  nr_srs_passive_metrics(&m.us, &m.dropped, &m.pregate, &m.backoff, &m.budget, &m.resets);
  return m;
}
}

class CellFeatSrs : public testing::Test {
 protected:
  PHY_VARS_NR_UE ue0, ue1;
  static void SetUpTestSuite() { srs_test_init(); }
  void SetUp() override
  {
    make_ue(&ue0, 512, 356, 24, 4);
    make_ue(&ue1, 1024, 718, 51, 2);
    nr_cell_bind(0);
    nr_srs_passive_cell_prealloc(0);
    nr_srs_passive_cell_prealloc(1);
  }
  void TearDown() override
  {
    for (int c = 1; c >= 0; c--) { nr_cell_bind(c); nr_srs_passive_stop(); }
    nr_cell_bind(0);
  }
  void start_both()
  {
    nr_cell_bind(0);
    nr_srs_passive_start(&ue0);
    nr_cell_bind(1);
    nr_srs_passive_start(&ue1);
  }
};

TEST_F(CellFeatSrs, EachCellHasItsOwnSearcher)
{
  nr_cell_bind(1);
  EXPECT_FALSE(nr_srs_passive_enabled());
  start_both();
  nr_cell_bind(0);
  ASSERT_TRUE(nr_srs_passive_enabled());
  nr_cell_bind(1);
  ASSERT_TRUE(nr_srs_passive_enabled());
  nr_srs_passive_stop();
  EXPECT_FALSE(nr_srs_passive_enabled());
  nr_cell_bind(0);
  EXPECT_TRUE(nr_srs_passive_enabled()) << "stopping cell 1 must not stop cell 0";
}

TEST_F(CellFeatSrs, EventOverflowAndBadCaptureCountOnTheirCellOnly)
{
  start_both();
  nr_cell_bind(1);
  const Dropped b1 = metrics();
  nr_cell_bind(0);
  const Dropped b0 = metrics();
  nr_cell_bind(1);
  for (int i = 0; i < 70; i++) nr_srs_passive_trigger(100 + i, 5, 1); /* 64 fit, 6 dropped (the worker is idle: no capture arrives) */
  ue1.frame_parms.ofdm_symbol_size = 2048; /* does not match the searcher grid: counted as a dropped capture */
  nr_srs_passive_capture(&ue1, 0, 5, 100);
  ue1.frame_parms.ofdm_symbol_size = 1024;
  /* the cell's own idle worker takes the lock for a moment every millisecond: a try-lock miss is a (counted) drop too, so allow two */
  EXPECT_GE(metrics().dropped - b1.dropped, 7u);
  EXPECT_LE(metrics().dropped - b1.dropped, 9u);
  nr_cell_bind(0);
  EXPECT_EQ(metrics().dropped - b0.dropped, 0u) << "cell 1's events and bad capture must not count on cell 0";
  nr_srs_passive_capture(&ue1, 0, 5, 100); /* cell 1's UE handed to cell 0: not its owner, silently ignored */
  EXPECT_EQ(metrics().dropped - b0.dropped, 0u);
}

TEST_F(CellFeatSrs, TwoBoundThreadsKeepSeparateCounters)
{
  start_both();
  Dropped b[2];
  for (int c = 0; c < 2; c++) { nr_cell_bind(c); b[c] = metrics(); }
  nr_cell_bind(0);
  const int n[2] = {66, 90};
  auto run = [&](int cell) {
    nr_cell_bind(cell);
    for (int i = 0; i < n[cell]; i++) nr_srs_passive_trigger(200 + i, 7, 1);
  };
  std::thread t0(run, 0), t1(run, 1);
  t0.join();
  t1.join();
  for (int c = 0; c < 2; c++) {
    nr_cell_bind(c);
    EXPECT_GE(metrics().dropped - b[c].dropped, uint64_t(n[c] - 64)) << "cell " << c;
    EXPECT_LE(metrics().dropped - b[c].dropped, uint64_t(n[c] - 64 + 2)) << "cell " << c;
  }
}

/* MPCI-INT: pause = the producers see the cell disabled (nothing queued, nothing counted), the searcher and the other cell stay; resume = enabled again. */
TEST_F(CellFeatSrs, PauseHidesTheCellFromProducersAndResumes)
{
  start_both();
  nr_cell_bind(1);
  const Dropped b1 = metrics();
  nr_srs_passive_cell_pause(1, true);
  EXPECT_FALSE(nr_srs_passive_enabled());
  for (int i = 0; i < 70; i++) nr_srs_passive_trigger(300 + i, 5, 1);
  EXPECT_LE(metrics().dropped - b1.dropped, 2u) << "a paused cell queues nothing (no overflow drops)";
  nr_cell_bind(0);
  EXPECT_TRUE(nr_srs_passive_enabled()) << "cell 0 unaffected";
  nr_srs_passive_cell_pause(1, false);
  nr_cell_bind(1);
  EXPECT_TRUE(nr_srs_passive_enabled());
  for (int i = 0; i < 70; i++) nr_srs_passive_trigger(400 + i, 5, 1);
  EXPECT_GE(metrics().dropped - b1.dropped, 6u) << "resumed: events are queued again (overflow counts)";
  nr_cell_bind(0);
}
