/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/*! \file openair1/PHY/NR_UE_ISAC/tools/isac_sync_replay.cc
 * \brief Cheap-office OTA sync bench (tests/ota_sync_bench/): runs the REAL
 * cpi_sto_tracker/cpi_cfo_tracker/cpi_sfo_tracker classes -- the exact production code
 * sensing_engine.cc calls, in the same order -- against a CFR grid built from an actual
 * two-SDR RF capture (tests/ota_sync_bench/process_capture.py), instead of the synthetic
 * hand-injected grids tests/isac_sync_test.cc uses. See tests/ota_sync_bench/README.md.
 *
 * Reads process_capture.py/grid_dump.py's binary format directly (documented in that file);
 * kept in sync with it by hand since this is a small, occasionally-run dev tool, not a
 * repo-wide interface.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "isac_sync.h"

extern "C" {
#include "common/utils/LOG/log.h"
#include "common/config/config_userapi.h"
}

extern "C" configmodule_interface_t* uniqCfg = nullptr;
extern "C" void exit_function(const char* file, const char* function, const int line, const char* s, const int assert)
{
  (void)file;
  (void)function;
  (void)line;
  (void)s;
  (void)assert;
  abort();
}

using namespace nr_isac;

namespace {

template <typename T>
T read_pod(std::ifstream& f)
{
  T v{};
  f.read(reinterpret_cast<char*>(&v), sizeof(T));
  return v;
}

struct grid_dump_t {
  uint32_t cpi_rows = 0, nof_subc = 0, nof_prb = 0, scs_hz = 0;
  uint64_t dl_center_hz = 0;
  uint16_t pci = 0, slots_per_frame = 0;
  double   nominal_los_range_m = 0.0;
  double   trusted_cfo_hz = 0.0, trusted_sfo_ppm = 0.0, trusted_mean_range_bin = 0.0;
  bool     has_injected_gt = false;
  double   injected_sto_samples = 0.0, injected_cfo_hz = 0.0, injected_sfo_ppm = 0.0;

  std::vector<uint32_t> row_comb;
  /* Not in the dump format: cpi_sto_tracker::process() gained a per-row illuminator argument
   * (a67f55faec) after this tool was written. The dumps are single-illuminator DL captures, so
   * every row is NR_ISAC_ILLUM_DL -- the value sensing_engine.cc initialises cpi_row_illum to.
   * Filled after load; the on-disk format is deliberately unchanged. */
  std::vector<uint8_t>  row_illum;
  std::vector<double>   row_time_slots;
  std::vector<icf_t>    h_cpi;
  std::vector<uint8_t>  occ_all;
};

bool load_grid_dump(const std::string& path, grid_dump_t& g)
{
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    fprintf(stderr, "cannot open %s\n", path.c_str());
    return false;
  }
  char magic[8];
  f.read(magic, sizeof(magic));
  if (std::memcmp(magic, "ISACOTA1", 8) != 0) {
    fprintf(stderr, "bad magic in %s (expected ISACOTA1)\n", path.c_str());
    return false;
  }
  g.cpi_rows = read_pod<uint32_t>(f);
  g.nof_subc = read_pod<uint32_t>(f);
  g.nof_prb  = read_pod<uint32_t>(f);
  g.scs_hz   = read_pod<uint32_t>(f);
  g.dl_center_hz = read_pod<uint64_t>(f);
  g.pci             = read_pod<uint16_t>(f);
  g.slots_per_frame = read_pod<uint16_t>(f);
  g.nominal_los_range_m  = read_pod<double>(f);
  g.trusted_cfo_hz       = read_pod<double>(f);
  g.trusted_sfo_ppm      = read_pod<double>(f);
  g.trusted_mean_range_bin = read_pod<double>(f);
  g.has_injected_gt = read_pod<uint8_t>(f) != 0;
  g.injected_sto_samples = read_pod<double>(f);
  g.injected_cfo_hz      = read_pod<double>(f);
  g.injected_sfo_ppm     = read_pod<double>(f);

  g.row_comb.resize(g.cpi_rows);
  f.read(reinterpret_cast<char*>(g.row_comb.data()), g.cpi_rows * sizeof(uint32_t));
  g.row_illum.assign(g.cpi_rows, (uint8_t)NR_ISAC_ILLUM_DL);
  g.row_time_slots.resize(g.cpi_rows);
  f.read(reinterpret_cast<char*>(g.row_time_slots.data()), g.cpi_rows * sizeof(double));

  const size_t nof_h = (size_t)g.cpi_rows * g.nof_subc;
  std::vector<float> h_interleaved(nof_h * 2);
  f.read(reinterpret_cast<char*>(h_interleaved.data()), h_interleaved.size() * sizeof(float));
  g.h_cpi.resize(nof_h);
  for (size_t i = 0; i < nof_h; i++) {
    g.h_cpi[i] = icf_t(h_interleaved[2 * i], h_interleaved[2 * i + 1]);
  }

  g.occ_all.resize(nof_h);
  f.read(reinterpret_cast<char*>(g.occ_all.data()), g.occ_all.size());

  if (!f) {
    fprintf(stderr, "short read on %s (truncated file?)\n", path.c_str());
    return false;
  }
  return true;
}

void print_row(const char* label, double value, const char* unit)
{
  printf("  %-28s %+.4f %s\n", label, value, unit);
}

} // namespace

int main(int argc, char** argv)
{
  std::string grid_path;
  bool        run_second_pass = true;

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--grid" && i + 1 < argc) {
      grid_path = argv[++i];
    } else if (arg == "--no-second-pass") {
      run_second_pass = false;
    } else if (arg == "-h" || arg == "--help") {
      printf("usage: %s --grid <grid_dump.bin> [--no-second-pass]\n", argv[0]);
      printf("  Runs the real cpi_sto_tracker/cpi_cfo_tracker/cpi_sfo_tracker against a\n"
             "  CFR grid built from an OTA capture (tests/ota_sync_bench/process_capture.py).\n"
             "  --no-second-pass skips the post-correction residual re-run.\n");
      return 0;
    } else {
      fprintf(stderr, "unknown arg: %s (use --help)\n", arg.c_str());
      return 1;
    }
  }
  if (grid_path.empty()) {
    fprintf(stderr, "missing --grid <path>. See --help.\n");
    return 1;
  }

  logInit();

  grid_dump_t g;
  if (!load_grid_dump(grid_path, g)) {
    return 1;
  }

  nr_isac_carrier_t carrier{};
  carrier.nof_prb         = g.nof_prb;
  carrier.scs_hz          = g.scs_hz;
  carrier.dl_center_hz    = g.dl_center_hz;
  carrier.pci             = g.pci;
  carrier.slots_per_frame = g.slots_per_frame;

  printf("loaded %s: %u rows x %u subcarriers (nof_prb=%u scs_hz=%u nominal_los_range_m=%.3f)\n",
         grid_path.c_str(), g.cpi_rows, g.nof_subc, g.nof_prb, g.scs_hz, g.nominal_los_range_m);

  printf("\n--- process_capture.py's independent (frequency-domain phase-difference) cross-check ---\n");
  print_row("trusted CFO", g.trusted_cfo_hz, "Hz");
  print_row("trusted SFO", g.trusted_sfo_ppm, "ppm");
  print_row("trusted mean range", g.trusted_mean_range_bin, "bins");

  if (g.has_injected_gt) {
    printf("\n--- Tier 1 injected ground truth (as passed to process_capture.py) ---\n");
    print_row("injected STO", g.injected_sto_samples, "samples (see README: compare deltas, not this run alone)");
    print_row("injected CFO", g.injected_cfo_hz, "Hz");
    print_row("injected SFO", g.injected_sfo_ppm, "ppm");
  }

  cpi_sto_tracker sto;
  sto_fit_result_t sto_fit = sto.process(g.h_cpi.data(), g.occ_all.data(), g.cpi_rows, g.nof_subc, g.row_comb.data(),
                                          g.row_illum.data(), g.row_time_slots.data(), carrier, /*sfo_ppm_hint=*/0.0,
                                          /*apply_corr=*/true, g.nominal_los_range_m);
  cpi_cfo_tracker cfo_tracker;
  cfo_fit_result_t cfo_fit = cfo_tracker.process(g.h_cpi.data(), g.occ_all.data(), g.nof_subc, sto.last_row_estimates());

  cpi_sfo_tracker sfo;
  sfo_fit_result_t sfo_fit = sfo.process(g.h_cpi.data(), g.occ_all.data(), g.cpi_rows, g.nof_subc, g.row_comb.data(),
                                          g.row_time_slots.data(), carrier, g.nominal_los_range_m);

  printf("\n--- real cpi_sto_tracker / cpi_cfo_tracker / cpi_sfo_tracker (production code) ---\n");
  printf("  STO: n_valid=%u n_flywheel=%u is_constant=%s mean_frac_bin=%+.4f slope_bins_per_s=%+.4f "
         "total_drift_bins=%+.4f\n",
         sto_fit.n_valid, sto_fit.n_flywheel, sto_fit.is_constant ? "true" : "false", sto_fit.mean_frac_bin,
         sto_fit.slope_bins_per_s, sto_fit.total_drift_bins);
  printf("  CFO: n_valid=%u cfo_hz=%+.4f cfo_hz_filtered=%+.4f residual_phase_rms_rad=%.4f\n", cfo_fit.n_valid,
         cfo_fit.cfo_hz, cfo_fit.cfo_hz_filtered, cfo_fit.residual_phase_rms_rad);
  printf("  SFO: n_candidate=%u n_excluded_isi=%u n_fit=%u sfo_ppm=%+.4f sfo_ppm_filtered=%+.4f r_squared=%.4f "
         "corrected=%s\n",
         sfo_fit.n_candidate, sfo_fit.n_excluded_isi, sfo_fit.n_fit, sfo_fit.sfo_ppm, sfo_fit.sfo_ppm_filtered,
         sfo_fit.r_squared, sfo_fit.corrected ? "true" : "false");

  printf("\n--- side-by-side vs. the trusted cross-check ---\n");
  printf("  CFO:  tracker=%+.4f Hz   trusted=%+.4f Hz   delta=%.4f Hz\n", cfo_fit.cfo_hz, g.trusted_cfo_hz,
         std::fabs(cfo_fit.cfo_hz - g.trusted_cfo_hz));
  printf("  SFO:  tracker=%+.4f ppm  trusted=%+.4f ppm  delta=%.4f ppm\n", sfo_fit.sfo_ppm, g.trusted_sfo_ppm,
         std::fabs(sfo_fit.sfo_ppm - g.trusted_sfo_ppm));

  if (run_second_pass) {
    // Mirrors isac_sync_test.cc's convention: h_cpi was corrected in place above, so re-running
    // fresh trackers on the same (now-corrected) grid measures the residual left behind --
    // should collapse toward 0 if the correction actually worked, not just "estimated something".
    cpi_sto_tracker  sto2;
    sto_fit_result_t sto_fit2 = sto2.process(g.h_cpi.data(), g.occ_all.data(), g.cpi_rows, g.nof_subc,
                                              g.row_comb.data(), g.row_illum.data(), g.row_time_slots.data(), carrier, 0.0, true,
                                              g.nominal_los_range_m);
    cpi_cfo_tracker  cfo_tracker2;
    cfo_fit_result_t cfo_fit2 =
        cfo_tracker2.process(g.h_cpi.data(), g.occ_all.data(), g.nof_subc, sto2.last_row_estimates());
    cpi_sfo_tracker  sfo2;
    sfo_fit_result_t sfo_fit2 = sfo2.process(g.h_cpi.data(), g.occ_all.data(), g.cpi_rows, g.nof_subc,
                                              g.row_comb.data(), g.row_time_slots.data(), carrier,
                                              g.nominal_los_range_m);
    printf("\n--- post-correction residual (re-running trackers on the now-corrected grid) ---\n");
    printf("  STO residual mean_frac_bin=%+.4f\n", sto_fit2.mean_frac_bin);
    printf("  CFO residual cfo_hz=%+.4f\n", cfo_fit2.cfo_hz);
    printf("  SFO residual sfo_ppm=%+.4f (corrected=%s)\n", sfo_fit2.sfo_ppm, sfo_fit2.corrected ? "true" : "false");
    printf("  (residuals well under the first-pass numbers above => the correction is doing real work)\n");
  }

  return 0;
}
