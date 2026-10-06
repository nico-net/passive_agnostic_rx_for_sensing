/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_csirs_disc_synth.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

extern "C" {
#include "nfapi/open-nFAPI/nfapi/public_inc/fapi_nr_ue_interface.h"
void *uniqCfg = nullptr;
void exit_function(const char *, const char *, int, const char *message, int)
{
  std::fprintf(stderr, "%s\n", message ? message : "");
  std::abort();
}
int nr_csirs_blind_rt_test_confirm_ring(const nr_csirs_disc_ring_t *,
                                        fapi_nr_dl_config_csirs_pdu_rel15_t *,
                                        uint16_t *,
                                        uint16_t *,
                                        int16_t *,
                                        int,
                                        uint32_t *);
int nr_csirs_blind_rt_test_candidate_count(void);
}

namespace {
struct Ring {
  nr_csirs_disc_ring_t ring = {};
  ~Ring() { nr_csirs_disc_synth_free(&ring); }
};

nr_csirs_disc_synth_cfg_t config(const nr_csirs_disc_synth_resource_t *resources, int n_resources)
{
  return {.n_slots = 640,
          .n_rb = 217,
          .n_ant = 4,
          .slots_per_frame = 20,
          .first_slot = 1000,
          .seed = 0x5a17c5d,
          .snr_db = 10,
          .load = 1,
          .dmrs = true,
          .ssb = true,
          .full_rank_mimo = true,
          .resources = resources,
          .n_resources = n_resources};
}

bool exact(const fapi_nr_dl_config_csirs_pdu_rel15_t &p,
           uint16_t period,
           uint16_t offset,
           const nr_csirs_disc_synth_resource_t &r)
{
  return p.row == r.row && p.freq_domain == r.fd_bitmap && p.symb_l0 == r.l0 && p.symb_l1 == r.l1
      && p.cdm_type == r.cdm && p.freq_density == r.density && p.start_rb == r.rb_start
      && p.nr_of_rbs == r.n_rb && p.scramb_id == r.scramb_id && period == r.period && offset == r.offset;
}
} // namespace

TEST(CsirsDiscE2E, ANoConfirmationOnDmrsOnlyCell)
{
  setenv("ISAC_CSIRS_DISC", "1", 1);
  setenv("ISAC_CSIRS_DISC_BACKEND", "cpu", 1);
  setenv("ISAC_CSIRS_DISC_SWEEP_HYPS", "32", 1);
  nr_csirs_disc_synth_init();
  Ring r;
  auto cfg = config(nullptr, 0);
  cfg.n_slots = 160;
  ASSERT_EQ(nr_csirs_disc_synth(&r.ring, &cfg), 0);
  nr_csirs_disc_process(&r.ring);
  EXPECT_EQ(nr_csirs_blind_rt_test_candidate_count(), 0);
}

TEST(CsirsDiscE2E, MixedRowsDiscoverConfirmAndExportExact)
{
  setenv("ISAC_CSIRS_DISC", "1", 1);
  setenv("ISAC_CSIRS_DISC_BACKEND", "cpu", 1);
  setenv("ISAC_CSIRS_DISC_SWEEP_HYPS", "64", 1);
  nr_csirs_disc_synth_init();
  nr_csirs_disc_synth_resource_t resources[] = {
      {.row = 1, .l0 = 4, .l1 = 0, .cdm = 0, .density = 3, .fd_bitmap = 1, .rb_start = 0,
       .n_rb = 52, .scramb_id = 415, .period = 10, .offset = 1, .power = 1.6},
      {.row = 2, .l0 = 2, .l1 = 0, .cdm = 0, .density = 2, .fd_bitmap = 8, .rb_start = 60,
       .n_rb = 24, .scramb_id = 101, .period = 5, .offset = 2, .power = 3.0},
      {.row = 4, .l0 = 6, .l1 = 0, .cdm = 1, .density = 2, .fd_bitmap = 2, .rb_start = 90,
       .n_rb = 28, .scramb_id = 202, .period = 8, .offset = 3, .power = 10.0},
      {.row = 6, .l0 = 8, .l1 = 0, .cdm = 1, .density = 2, .fd_bitmap = 15, .rb_start = 120,
       .n_rb = 28, .scramb_id = 303, .period = 10, .offset = 4, .power = 10.0},
      {.row = 11, .l0 = 10, .l1 = 0, .cdm = 1, .density = 2, .fd_bitmap = 15, .rb_start = 150,
       .n_rb = 28, .scramb_id = 404, .period = 5, .offset = 1, .power = 3.0},
      {.row = 16, .l0 = 4, .l1 = 8, .cdm = 1, .density = 2, .fd_bitmap = 15, .rb_start = 180,
       .n_rb = 28, .scramb_id = 505, .period = 8, .offset = 5, .power = 3.0},
  };
  Ring r;
  auto cfg = config(resources, 6);
  ASSERT_EQ(nr_csirs_disc_synth(&r.ring, &cfg), 0);

  const auto begin = std::chrono::steady_clock::now();
  nr_csirs_disc_process(&r.ring);
  const double batch_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
  nr_csirs_disc_metrics_t metrics = {};
  nr_csirs_disc_get_metrics(&metrics);
  std::printf("pipeline footprints=%llu hyps=%llu winners=%llu ambiguous=%llu\n",
              (unsigned long long)metrics.footprints,
              (unsigned long long)metrics.hyps,
              (unsigned long long)metrics.winners,
              (unsigned long long)metrics.ambiguous);
  ASSERT_EQ(nr_csirs_blind_rt_test_candidate_count(), 6);

  fapi_nr_dl_config_csirs_pdu_rel15_t pdu[8] = {};
  uint16_t period[8] = {}, offset[8] = {};
  int16_t delta[8] = {};
  uint32_t slots = 0;
  const int n = nr_csirs_blind_rt_test_confirm_ring(&r.ring, pdu, period, offset, delta, 8, &slots);
  ASSERT_EQ(n, 6);
  for (const auto &resource : resources) {
    bool found = false;
    for (int k = 0; k < n; ++k)
      found |= exact(pdu[k], period[k], offset[k], resource) && delta[k] == 0;
    EXPECT_TRUE(found) << "row=" << static_cast<int>(resource.row);
  }
  EXPECT_GE(metrics.winners, 6u);
  EXPECT_GE(metrics.cpu_batches, 1u);
  std::printf("CSI-RS discovery CPU batch %.3f ms; slots-to-confirm=%u\n", batch_ms, slots);
}
