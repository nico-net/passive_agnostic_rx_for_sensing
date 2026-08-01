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

/*! \file openair1/PHY/NR_UE_ISAC/nr_isac.cc
 * \brief Public C API glue for the OAI-UE ISAC sensing pipeline: reads the [sensing]
 * config section, owns the process-wide engine instance, and forwards RT taps.
 */

#include "nr_isac.h"
#include "defs_nr_UE_ISAC.h"
#include "detection_report.h"
#include "isac_aoa.h"
#include "sensing_engine.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
}

using namespace nr_isac;

namespace {

// Process-wide sensing state. The engine is created only when [sensing] enable is set.
std::unique_ptr<sensing_engine> g_engine;
nr_isac_args_t                  g_args;
std::atomic<bool>               g_enabled{false};
std::atomic<bool>               g_started{false};
/// Element count parsed from [sensing] rx_array, or 0 when AoA is off. Read by the RT taps via
/// nr_isac_aoa_antennas() to decide how many antennas to extract Ĥ for.
uint32_t                        g_aoa_antennas = 0;

nr_isac_source_t parse_source(const char* s)
{
  if (s == nullptr) {
    return NR_ISAC_SRC_CSI_RS;
  }
  if (std::strcmp(s, "pdsch_dmrs") == 0) {
    return NR_ISAC_SRC_PDSCH_DMRS;
  }
  if (std::strcmp(s, "pdsch_data") == 0) {
    return NR_ISAC_SRC_PDSCH_DATA;
  }
  return NR_ISAC_SRC_CSI_RS;
}

// Map one whitespace-trimmed token to its source bit; returns 0 (no bit) for an empty/unknown token.
uint32_t source_bit_from_token(const std::string& tok)
{
  if (tok == "csi_rs") {
    return 1u << NR_ISAC_SRC_CSI_RS;
  }
  if (tok == "pdsch_dmrs") {
    return 1u << NR_ISAC_SRC_PDSCH_DMRS;
  }
  if (tok == "pdsch_data") {
    return 1u << NR_ISAC_SRC_PDSCH_DATA;
  }
  return 0;
}

// Parse a comma-separated enabled-source set ("csi_rs,pdsch_dmrs"). Empty string -> mask 0 so the
// caller can fall back to the single legacy `source`.
uint32_t parse_sources_mask(const char* s)
{
  uint32_t mask = 0;
  if (s == nullptr) {
    return 0;
  }
  std::string in(s);
  size_t      pos = 0;
  while (pos < in.size()) {
    size_t      comma = in.find(',', pos);
    std::string tok   = in.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    pos               = (comma == std::string::npos) ? in.size() : comma + 1;
    // trim surrounding whitespace
    size_t b = tok.find_first_not_of(" \t");
    size_t e = tok.find_last_not_of(" \t");
    if (b == std::string::npos) {
      continue;
    }
    tok = tok.substr(b, e - b + 1);
    const uint32_t bit = source_bit_from_token(tok);
    if (bit != 0) {
      mask |= bit;
    } else {
      LOG_W(PHY, "SENSING: ignoring unknown source '%s' in sensing.sources\n", tok.c_str());
    }
  }
  return mask;
}

// Lowest-numbered enabled source in the mask (the "primary"); defaults to CSI-RS for an empty mask.
nr_isac_source_t primary_source(uint32_t mask)
{
  for (int i = 0; i < NR_ISAC_SRC_COUNT; i++) {
    if (mask & (1u << i)) {
      return (nr_isac_source_t)i;
    }
  }
  return NR_ISAC_SRC_CSI_RS;
}

// paramdef_t uses anonymous unions, which C++ cannot populate with C-style designated initializers.
// These helpers zero-init an entry and set the union pointer/default fields by name instead.
paramdef_t mk_int(const char* name, const char* help, unsigned flags, int* ptr, int defv)
{
  paramdef_t p;
  std::memset(&p, 0, sizeof(p));
  std::strncpy(p.optname, name, sizeof(p.optname) - 1);
  p.helpstr    = help;
  p.paramflags = flags;
  p.iptr       = ptr;
  p.defintval  = defv;
  p.type       = TYPE_INT;
  return p;
}
paramdef_t mk_dbl(const char* name, const char* help, double* ptr, double defv)
{
  paramdef_t p;
  std::memset(&p, 0, sizeof(p));
  std::strncpy(p.optname, name, sizeof(p.optname) - 1);
  p.helpstr   = help;
  p.dblptr    = ptr;
  p.defdblval = defv;
  p.type      = TYPE_DOUBLE;
  return p;
}
paramdef_t mk_str(const char* name, const char* help, char** ptr, const char* defv)
{
  paramdef_t p;
  std::memset(&p, 0, sizeof(p));
  std::strncpy(p.optname, name, sizeof(p.optname) - 1);
  p.helpstr   = help;
  p.strptr    = ptr;
  p.defstrval = defv;
  p.type      = TYPE_STRING;
  return p;
}

} // namespace

extern "C" void nr_isac_init(void)
{
  if (g_enabled.load()) {
    return; // already initialised
  }

  // Local storage for the [sensing] config section. Defaults mirror nr_isac_args_t / srsUE.
  int    p_enable = 0, p_interp = 1, p_capture = 0, p_selftest = 0, p_sync_correction = 1;
  int    p_sync_sto = 1, p_sync_cfo = 1, p_sync_sfo = 1, p_sync_los = 1;
  int    p_cpi_slots = 256;
  int    p_cfar_guard = 4, p_cfar_train = 8, p_cfar_per_column = 0, p_cfar_per_row = 0;
  int    p_subbin_interp = 0;
  int    p_gating_reject = 0, p_gating_max_off = 3, p_gating_tol = 2;
  double p_gating_min_rel = 0.35, p_gating_snr_margin = 3.0;
  double p_cfar_pfa = 0.0, p_cfar_target_fa = 1.0;
  int    p_zdg = 3, p_zrg = 2, p_nms_r = 3, p_nms_d = 3, p_maxdet = 32;
  int    p_conj_reject = 1, p_conj_guard = 4, p_range_whiten = 0, p_doppler_nudft = 0;
  int    p_mc_enable = 0, p_mc_rank = 0, p_mc_rank_max = 8, p_mc_iters = 15, p_mc_power = 1;
  int    p_sd_enable = 0, p_sd_iters = 40;
  double p_sd_lambda_scale = 1.0, p_sd_peak_ratio = 0.05;
  double p_sd_lambda_min = 0.05, p_sd_lambda_max = 1.5, p_sd_adapt_rate = 0.85;
  int    p_sd_target_min = 1, p_sd_target_max = 6;
  double p_sd_harmonic_ratio = 0.25;
  int    p_harm_reject = 0, p_harm_guard = 4, p_harm_maxk = 4;
  double p_harm_tol = 0.15, p_harm_snr_margin = 6.0;
  double p_max_range = 0.0;
  int    p_adapt_guard = 0;
  int    p_dq_adapt = 0;
  double p_dq_cost = 1.0;
  int    p_far_harm_reject = 0;
  double p_far_harm_far = 0.0, p_far_harm_near = 0.0;
  int    p_harm_pos_reject = 0;
  double p_harm_pos_chi2 = 9.0, p_harm_pos_range_tol = 8.0;
  int    p_cfar_fa_adapt = 0;
  double p_cfar_fa_min = 0.5, p_cfar_fa_max = 20.0, p_cfar_fa_rate = 1.15;
  int    p_cfar_fa_tmin = 2, p_cfar_fa_tmax = 8;
  int    p_cpi_qgate = 0;
  double p_cpi_qratio = 1.5, p_cpi_qalpha = 0.2;
  double p_eca_delay_max = 0.0, p_eca_dopp_max = 0.5;
  double p_range_window_sll = 60.0;
  double p_nominal_los_range = 98.0;
  int    p_track_enable = 0, p_track_max_coast = 5, p_track_q_adapt = 1;
  double p_track_init_acc = 100.0;
  char*  p_track_model = nullptr;
  double p_track_r = 2.6, p_track_q = 0.0025, p_track_gate = 3.0, p_track_vvar = 25.0, p_track_rv = 1.0;
  double p_track_nis_alpha = 0.3, p_track_q_mult_max = 30.0, p_track_adapt_win = 8.0;
  int    p_track_max_tracks = 16, p_track_confirm_m = 0, p_track_confirm_n = 5;
  double p_track_assoc_gate = 5.0, p_track_confirm_pfa = 1e-3;
  double p_track_assoc_max_sr = 15.0, p_track_assoc_max_sv = 10.0;
  int    p_track_harm_reject = 0;
  double p_track_harm_range = 10.0;
  int    p_track_flicker_reject = 0, p_track_flicker_min_upd = 3;
  double p_track_flicker_max_db = 6.0, p_track_flicker_alpha = 0.4;
  double p_rx_x = 0.0, p_rx_y = 0.0, p_tx_x = 0.0, p_tx_y = 0.0;
  char*  p_source     = nullptr;
  char*  p_clutter    = nullptr;
  char*  p_range_win  = nullptr;
  char*  p_sources    = nullptr;
  char*  p_detector   = nullptr;
  int    p_mf_per_row_norm = 1;
  int    p_clean_deconv = 0, p_clean_max_comp = 0, p_clean_max_comp_cap = 16, p_clean_restore = 1;
  int    p_clean_occ_aware = 0;
  int    p_subslot_symbols = 0, p_subslot_min_re = 600;
  double p_subslot_min_snr_db = 0.0;
  double p_clean_loop_gain = 0.8, p_clean_stop_db = 25.0;
  char*  p_targets    = nullptr;
  char*  p_selftest_los = nullptr;
  int    p_aoa_enable = 0, p_aoa_selfcal = 0, p_aoa_search = 2;
  double p_aoa_boresight = 0.0, p_aoa_scan_step = 1.0, p_aoa_min_snr = 6.0, p_aoa_broadside = -1000.0;
  char*  p_rx_array   = nullptr;
  char*  p_aoa_est    = nullptr;
  char*  p_out_path   = nullptr;
  char*  p_rx_id      = nullptr;
  char*  p_illum_id   = nullptr;
  char*  p_report     = nullptr;
  char*  p_endpoint   = nullptr;

  paramdef_t params[] = {
      mk_int("enable", "enable ISAC sensing", PARAMFLAG_BOOL, &p_enable, 0),
      mk_str("source", "single source (legacy): csi_rs|pdsch_dmrs|pdsch_data", &p_source, "csi_rs"),
      mk_str("sources", "CFR-fusion enabled set, comma list e.g. \"csi_rs,pdsch_dmrs\" (overrides source)", &p_sources, ""),
      mk_int("cpi_slots", "coherent processing interval", 0, &p_cpi_slots, 256),
      mk_int("interpolate", "fill comb gaps + resample", PARAMFLAG_BOOL, &p_interp, 1),
      mk_int("capture", "dump RVM raster + rvm_blob", PARAMFLAG_BOOL, &p_capture, 0),
      mk_int("selftest", "inject a default synthetic echo", PARAMFLAG_BOOL, &p_selftest, 0),
      mk_str("selftest_targets", "DELAY_US:DOPPLER_HZ:GAIN,...", &p_targets, ""),
      mk_str("selftest_los", "known LOS-path impairment for the offline sync self-test: STO_US:CFO_HZ:SFO_PPM",
             &p_selftest_los, ""),
      mk_int("sync_correction", "enable Phase 1-4 STO/CFO/SFO/closed-loop LOS correction", PARAMFLAG_BOOL,
             &p_sync_correction, 1),
      mk_int("sync_sto", "Phase 1 fine-STO correction (under sync_correction)", PARAMFLAG_BOOL, &p_sync_sto, 1),
      mk_int("sync_cfo", "Phase 2 residual-CFO/CPE correction (under sync_correction)", PARAMFLAG_BOOL, &p_sync_cfo, 1),
      mk_int("sync_sfo", "Phase 3 SFO correction (under sync_correction)", PARAMFLAG_BOOL, &p_sync_sfo, 1),
      mk_int("sync_los", "Phase 4 closed-loop LOS bias (under sync_correction)", PARAMFLAG_BOOL, &p_sync_los, 1),
      mk_dbl("nominal_los_range_m", "expected LOS differential range (m); 0 when the direct path is at dR=0",
             &p_nominal_los_range, 98.0),
      mk_int("cfar_guard", "CA-CFAR guard cells per side", 0, &p_cfar_guard, 4),
      mk_int("cfar_train", "CA-CFAR training cells per side", 0, &p_cfar_train, 8),
      mk_int("cfar_per_column", "also require a per-Doppler-column (velocity-lane) CFAR test; rejects a strong target's range-wide pedestal", PARAMFLAG_BOOL, &p_cfar_per_column, 0),
      mk_int("cfar_per_row", "also require a per-RANGE-row (Doppler-lane) CFAR test; rejects the slow-time ridge a strong target smears across its own range row", PARAMFLAG_BOOL, &p_cfar_per_row, 0),
      mk_int("subbin_interp", "sub-bin parabolic peak interpolation in range and Doppler (removes half-bin quantisation error from every detection)", PARAMFLAG_BOOL, &p_subbin_interp, 0),
      mk_int("gating_reject", "reject Doppler replicas at the MEASURED scheduling-gate offsets (from the per-CPI row-energy spectrum) rather than assumed integer multiples", PARAMFLAG_BOOL, &p_gating_reject, 0),
      mk_int("gating_max_offsets", "how many envelope-spectrum peaks to treat as gating offsets", 0, &p_gating_max_off, 3),
      mk_dbl("gating_min_rel", "envelope peak must reach this fraction of the strongest peak", &p_gating_min_rel, 0.35),
      mk_int("gating_tol_bins", "Doppler-bin tolerance when matching a gating offset", 0, &p_gating_tol, 2),
      mk_dbl("gating_snr_margin", "vetoing same-range neighbour must be this many dB stronger", &p_gating_snr_margin, 3.0),
      mk_dbl("cfar_pfa", "CA-CFAR per-cell false-alarm probability; <=0 (default)=auto-derive from cfar_target_fa_per_cpi", &p_cfar_pfa, 0.0),
      mk_dbl("cfar_target_fa_per_cpi", "auto mode only: target mean CA-CFAR false alarms per CPI across the whole grid", &p_cfar_target_fa, 1.0),
      mk_int("zero_doppler_guard", "Doppler notch half-width (bins)", 0, &p_zdg, 3),
      mk_int("zero_range_guard", "range notch half-width (bins)", 0, &p_zrg, 2),
      mk_int("nms_range_bins", "NMS radius in range bins; 0 = AUTO from the range window's mainlobe half-width", 0, &p_nms_r, 0),
      mk_int("nms_doppler_bins", "NMS radius in Doppler bins; 0 = AUTO from the slow-time (Hann) mainlobe half-width", 0, &p_nms_d, 0),
      mk_int("max_detections", "cap on detections per CPI", 0, &p_maxdet, 32),
      mk_int("conj_image_reject", "reject conjugate-image (mirror ghost) detections", PARAMFLAG_BOOL, &p_conj_reject, 1),
      mk_int("conj_image_guard", "range-bin tolerance when matching a detection to its mirror", 0, &p_conj_guard, 4),
      mk_int("harmonic_reject", "reject Doppler-harmonic ghosts (same range, integer-multiple velocity of a stronger detection)", PARAMFLAG_BOOL, &p_harm_reject, 0),
      mk_int("harmonic_guard", "range-bin tolerance when pairing a harmonic to its fundamental", 0, &p_harm_guard, 4),
      mk_int("harmonic_max_k", "highest Doppler-harmonic order to reject (2..k)", 0, &p_harm_maxk, 4),
      mk_dbl("harmonic_tol", "fractional tolerance on the integer velocity ratio; 0 = AUTO (per-pair, from each detection's own SNR-derived rate sigma)", &p_harm_tol, 0.0),
      mk_dbl("harmonic_snr_margin", "max dB the fundamental may be weaker than its harmonic and still reject it", &p_harm_snr_margin, 6.0),
      mk_int("harmonic_pos_reject", "reject a Doppler harmonic anchored on the AoA-derived POSITION of its fundamental instead of its range bin (needs bearings; inert without an array)", PARAMFLAG_BOOL, &p_harm_pos_reject, 0),
      mk_dbl("harmonic_pos_chi2", "chi2 threshold (1 dof, 9=3sigma) on the tangential position residual, normalised by each detection's own azimuth_std_deg", &p_harm_pos_chi2, 9.0),
      mk_dbl("harmonic_pos_range_tol_m", "flat tolerance (m) on the RADIAL position residual only (no per-detection range sigma exists to chi2-ize it)", &p_harm_pos_range_tol, 8.0),
      mk_int("det_quality_adapt", "adaptive per-detection P(real) gate learned online (robust SNR null + temporal persistence); no dB threshold", PARAMFLAG_BOOL, &p_dq_adapt, 0),
      mk_dbl("det_quality_cost_ratio", "relative cost of admitting a false alarm vs losing a real detection (1 = symmetric Bayes)", &p_dq_cost, 1.0),
      mk_int("adaptive_clutter_guard", "derive zero_range_guard/zero_doppler_guard from each CPI's own clutter profile", PARAMFLAG_BOOL, &p_adapt_guard, 0),
      mk_dbl("max_range_m", "notch every range bin beyond this BEFORE CFAR; >0 = explicit, <0 = AUTO from the range profile's own noise floor, 0 = disabled", &p_max_range, 0.0),
      mk_int("far_harmonic_reject", "reject far-range detections at k*velocity of a dominant near-range Doppler component (range-smeared amplitude-gating pedestal)", PARAMFLAG_BOOL, &p_far_harm_reject, 0),
      mk_dbl("far_harmonic_far_m", "range beyond which a detection is a far-ghost candidate", &p_far_harm_far, 0.0),
      mk_dbl("far_harmonic_near_m", "range within which dominant Doppler components are sought", &p_far_harm_near, 0.0),
      mk_int("cfar_fa_adapt_enable", "closed-loop adaptation of cfar_target_fa_per_cpi from the measured raw detection count (seed unchanged, only the loop is new)", PARAMFLAG_BOOL, &p_cfar_fa_adapt, 0),
      mk_dbl("cfar_fa_min", "floor for the adapted cfar_target_fa_per_cpi", &p_cfar_fa_min, 0.5),
      mk_dbl("cfar_fa_max", "ceiling for the adapted cfar_target_fa_per_cpi", &p_cfar_fa_max, 20.0),
      mk_dbl("cfar_fa_adapt_rate", "per-CPI multiplicative step for the cfar_target_fa_per_cpi adaptation", &p_cfar_fa_rate, 1.15),
      mk_int("cfar_fa_target_min_det", "below this many raw detections/CPI, raise cfar_target_fa_per_cpi", 0, &p_cfar_fa_tmin, 2),
      mk_int("cfar_fa_target_max_det", "above this many raw detections/CPI, lower cfar_target_fa_per_cpi", 0, &p_cfar_fa_tmax, 8),
      mk_int("cpi_quality_gate", "drop all detections from a starved CPI whose T_slot >> running EMA (lets the tracker coast)", PARAMFLAG_BOOL, &p_cpi_qgate, 0),
      mk_dbl("cpi_quality_max_ratio", "gate a CPI whose T_slot exceeds this x the running EMA", &p_cpi_qratio, 1.5),
      mk_dbl("cpi_quality_ema_alpha", "EMA smoothing factor for the running typical T_slot", &p_cpi_qalpha, 0.2),
      mk_str("clutter_removal", "clutter cancellation method: mean|eca+", &p_clutter, "mean"),
      mk_dbl("eca_delay_max_m", "ECA delay removal window [0,x] m (<=0 => full range)", &p_eca_delay_max, 0.0),
      mk_dbl("eca_doppler_max_mps", "ECA Doppler removal half-band [-x,+x] m/s", &p_eca_dopp_max, 0.5),
      mk_str("detector", "detector front-end: fft|matched_filter (matched filter = occupancy-matched hypothesis bank, artifact-free by construction)", &p_detector, "fft"),
      mk_int("mf_per_row_norm", "matched_filter: energy-normalise each row by its occupancy (removes amplitude-gating harmonics)", PARAMFLAG_BOOL, &p_mf_per_row_norm, 1),
      mk_int("clean_deconv", "CLEAN deconvolution: coherently strip each strong scatterer's window PSF (range pedestal + Doppler sidelobes) before CFAR (detector=fft, uniform comb only)", PARAMFLAG_BOOL, &p_clean_deconv, 0),
      mk_int("subslot_symbols", "sub-slot CFR sampling: target OFDM symbols per slow-time row (0=off, one row/slot). Raises the unambiguous velocity ~N-fold", 0, &p_subslot_symbols, 0),
      mk_int("subslot_min_re", "sub-slot SPARSITY gate: min REs for a row to stand alone (else it absorbs the next symbol)", 0, &p_subslot_min_re, 600),
      mk_dbl("subslot_min_snr_db", "sub-slot SNR gate (dB); <= 0 = AUTO: full-slot SNR minus 10*log10(groups)", &p_subslot_min_snr_db, 0.0),
      mk_int("clean_occ_aware", "CLEAN: model each component's PSF through the ACTUAL per-row occupancy mask (removes amplitude-gating Doppler-harmonic replicas; needs clean_deconv=1, uses raw occ grid, costly)", PARAMFLAG_BOOL, &p_clean_occ_aware, 0),
      mk_int("clean_max_components", "CLEAN component budget per CPI; 0=auto-derive from previous CPI's detection count (recommended)", 0, &p_clean_max_comp, 0),
      mk_int("clean_max_components_cap", "CLEAN auto mode: hard ceiling on the derived component budget", 0, &p_clean_max_comp_cap, 16),
      mk_dbl("clean_loop_gain", "CLEAN loop gain gamma (0,1]: fraction of the peak subtracted per iteration", &p_clean_loop_gain, 0.8),
      mk_dbl("clean_stop_db", "CLEAN stop threshold: residual peak this many dB below the initial peak", &p_clean_stop_db, 25.0),
      mk_int("clean_restore_bins", "CLEAN clean-beam half-width (bins) for restored components; 0=single-bin delta", 0, &p_clean_restore, 1),
      mk_str("range_window", "fast-time (range) window: hann|chebyshev", &p_range_win, "hann"),
      mk_dbl("range_window_sidelobe_db", "Dolph-Chebyshev equiripple sidelobe level (dB, positive)",
             &p_range_window_sll, 60.0),
      mk_int("range_whiten", "soft spectral whitening: attenuate subcarriers hotter than the median slow-time RMS (kills the sparse-occupancy range pedestal)", PARAMFLAG_BOOL, &p_range_whiten, 0),
      mk_int("doppler_nudft", "non-uniform Doppler DFT over actual row times (removes irregular-sampling Doppler harmonics at the source; bypasses slow-time resampling)", PARAMFLAG_BOOL, &p_doppler_nudft, 0),
      mk_int("slow_time_complete", "low-rank matrix completion of the CFR grid before range/Doppler (removes amplitude-gating harmonics at the source)", PARAMFLAG_BOOL, &p_mc_enable, 0),
      mk_int("mc_rank", "matrix-completion target rank; 0=auto-derive from previous CPI's detection count (recommended)", 0, &p_mc_rank, 0),
      mk_int("mc_rank_max", "auto mode only: ceiling on the derived completion rank", 0, &p_mc_rank_max, 8),
      mk_int("mc_iters", "matrix-completion SVP iterations", 0, &p_mc_iters, 15),
      mk_int("mc_power_iters", "matrix-completion extra subspace power iterations per projection", 0, &p_mc_power, 1),
      mk_int("doppler_sparse", "L1/FISTA sparse Doppler verification on CFAR-flagged range bins (rejects dense-transform harmonic leakage)", PARAMFLAG_BOOL, &p_sd_enable, 0),
      mk_int("doppler_sparse_iters", "FISTA iterations per verified range bin", 0, &p_sd_iters, 40),
      mk_dbl("doppler_sparse_lambda_scale", "multiplier on the auto universal L1 threshold", &p_sd_lambda_scale, 1.0),
      mk_dbl("doppler_sparse_peak_ratio", "min fraction of a row's max sparse power for a detection to survive", &p_sd_peak_ratio, 0.05),
      mk_dbl("doppler_sparse_harmonic_ratio", "min sub-harmonic/max sparse power fraction to reject a detection as an orphaned harmonic (stricter than peak_ratio)", &p_sd_harmonic_ratio, 0.25),
      mk_dbl("doppler_sparse_lambda_min", "adaptive lambda_scale floor", &p_sd_lambda_min, 0.05),
      mk_dbl("doppler_sparse_lambda_max", "adaptive lambda_scale ceiling", &p_sd_lambda_max, 1.5),
      mk_dbl("doppler_sparse_adapt_rate", "per-CPI multiplicative adaptation step (<1)", &p_sd_adapt_rate, 0.85),
      mk_int("doppler_sparse_target_min_det", "below this many surviving detections/CPI, relax lambda_scale", 0, &p_sd_target_min, 1),
      mk_int("doppler_sparse_target_max_det", "above this many surviving detections/CPI, tighten lambda_scale", 0, &p_sd_target_max, 6),
      mk_int("track_enable", "enable the per-CPI Kalman target track", PARAMFLAG_BOOL, &p_track_enable, 0),
      mk_str("track_model", "tracker motion model: cv|ca", &p_track_model, "cv"),
      mk_dbl("track_init_acc_var", "CA initial acceleration variance ((m/s^2)^2)", &p_track_init_acc, 100.0),
      mk_dbl("track_r_var_m2", "KF range measurement variance R (m^2)", &p_track_r, 2.6),
      mk_dbl("track_rv_var_m2s2", "KF range-rate (Doppler) measurement variance ((m/s)^2)", &p_track_rv, 1.0),
      mk_dbl("track_q_accel", "KF accel PSD q ((m/s^2)^2/Hz)", &p_track_q, 0.0025),
      mk_dbl("track_gate_sigma", "KF association gate (sigmas)", &p_track_gate, 3.0),
      mk_dbl("track_init_vel_var", "KF initial range-rate variance (m/s)^2", &p_track_vvar, 25.0),
      mk_int("track_max_coast", "consecutive coasted CPIs before dropping the track", 0, &p_track_max_coast, 5),
      mk_int("track_q_adapt", "adapt q from the NIS EWMA (no per-route tuning needed)", PARAMFLAG_BOOL, &p_track_q_adapt, 1),
      mk_dbl("track_nis_ewma_alpha", "NIS EWMA smoothing", &p_track_nis_alpha, 0.3),
      mk_dbl("track_q_mult_max", "cap on the NIS-driven q inflation factor", &p_track_q_mult_max, 30.0),
      mk_dbl("track_q_adapt_window_sigma", "near-miss window (sigmas) that still feeds the NIS EWMA", &p_track_adapt_win, 8.0),
      mk_int("track_max_tracks", "MOT: cap on simultaneous tracks (tentative+confirmed)", 0, &p_track_max_tracks, 16),
      mk_int("track_confirm_m", "MOT: M hits within N CPIs to confirm a track (M-of-N); 0=auto-derive from measured false-alarm density (recommended)", 0, &p_track_confirm_m, 0),
      mk_int("track_confirm_n", "MOT: N-CPI sliding window for the M-of-N confirmation", 0, &p_track_confirm_n, 5),
      mk_dbl("track_confirm_target_pfa", "MOT: auto mode (track_confirm_m=0) target false-confirm probability within the N-CPI window", &p_track_confirm_pfa, 1e-3),
      mk_dbl("track_assoc_gate_sigma", "MOT: association gate (sigmas) for detection<->track routing", &p_track_assoc_gate, 5.0),
      mk_int("track_harmonic_reject", "MOT: drop a confirmed track at integer-multiple velocity of a same-range track (leaked Doppler harmonic)", PARAMFLAG_BOOL, &p_track_harm_reject, 0),
      mk_dbl("track_harmonic_range_m", "MOT: max range separation (m) to treat two tracks as co-located for harmonic rejection", &p_track_harm_range, 10.0),
      mk_int("track_flicker_reject", "MOT: withhold a confirmed track whose per-CPI SNR jitter (power flicker) is high -- a gated-harmonic ghost signature (TBD energy consistency)", PARAMFLAG_BOOL, &p_track_flicker_reject, 0),
      mk_dbl("track_flicker_max_db", "MOT: max EWMA |dSNR_dB|/update for a track to be reported", &p_track_flicker_max_db, 6.0),
      mk_int("track_flicker_min_updates", "MOT: min accepted detections before the flicker gate applies", 0, &p_track_flicker_min_upd, 3),
      mk_dbl("track_flicker_ewma_alpha", "MOT: EWMA smoothing for the per-track SNR-jitter estimate", &p_track_flicker_alpha, 0.4),
      mk_dbl("track_assoc_gate_max_sr_m", "MOT: hard ceiling on the association gate's range radius (m), independent of adaptive-q-inflated sigma", &p_track_assoc_max_sr, 15.0),
      mk_dbl("track_assoc_gate_max_sv_mps", "MOT: hard ceiling on the association gate's range-rate radius (m/s)", &p_track_assoc_max_sv, 10.0),
      mk_int("aoa_enable", "estimate a per-detection bearing from a receive antenna array", PARAMFLAG_BOOL,
             &p_aoa_enable, 0),
      mk_str("rx_array", "receive element offsets \"x,y;x,y;...\" (m, array frame)", &p_rx_array, ""),
      mk_dbl("rx_array_boresight_deg", "rotation of the array frame into ENU (deg CCW from east)",
             &p_aoa_boresight, 0.0),
      mk_str("aoa_estimator", "beamscan|interferometry|music", &p_aoa_est, "beamscan"),
      mk_dbl("aoa_scan_step_deg", "manifold scan step for beamscan/MUSIC (deg)", &p_aoa_scan_step, 1.0),
      mk_dbl("aoa_min_snr_db", "min detection SNR to report a bearing at all", &p_aoa_min_snr, 6.0),
      mk_int("aoa_cell_search_bins", "half-width of the local re-peak search around each detection cell", 0,
             &p_aoa_search, 2),
      mk_dbl("aoa_broadside_deg", "ENU direction the array faces; picks the half-plane a LINEAR array scans (-1000 = the array normal)",
             &p_aoa_broadside, -1000.0),
      mk_int("aoa_selfcal", "calibrate per-channel phase against the known-bearing direct path", PARAMFLAG_BOOL,
             &p_aoa_selfcal, 0),
      mk_str("out_path", "output path prefix", &p_out_path, "/tmp/oaiue_sensing"),
      mk_str("rx_id", "logical receiver id", &p_rx_id, "rx1"),
      mk_dbl("rx_pos_x", "receiver ENU x (m)", &p_rx_x, 0.0),
      mk_dbl("rx_pos_y", "receiver ENU y (m)", &p_rx_y, 0.0),
      mk_str("illuminator_id", "logical illuminator id", &p_illum_id, "gnb1"),
      mk_dbl("tx_pos_x", "transmitter ENU x (m)", &p_tx_x, 0.0),
      mk_dbl("tx_pos_y", "transmitter ENU y (m)", &p_tx_y, 0.0),
      mk_str("report_path", "DetectionReport JSON-lines path", &p_report, ""),
      mk_str("report_endpoint", "ZeroMQ PUB bind endpoint", &p_endpoint, ""),
  };

  config_get(config_get_if(), params, (int)(sizeof(params) / sizeof(params[0])), "sensing");

  if (!p_enable) {
    LOG_I(PHY, "SENSING: [sensing] section disabled (enable=0); ISAC pipeline not started\n");
    return;
  }

  g_args                    = nr_isac_args_t();
  g_args.enable             = true;
  // Enabled-source set: the multi-source "sources" list wins when non-empty; otherwise fall back to
  // the legacy single "source" so existing single-source configs keep their exact behaviour.
  uint32_t sources_mask = parse_sources_mask(p_sources);
  if (sources_mask == 0) {
    sources_mask = 1u << parse_source(p_source);
  }
  g_args.sources_mask       = sources_mask;
  g_args.source             = primary_source(sources_mask);
  g_args.cpi_slots          = (uint32_t)(p_cpi_slots > 0 ? p_cpi_slots : 1);
  g_args.interpolate        = p_interp != 0;
  g_args.capture_enable     = p_capture != 0;
  g_args.selftest           = p_selftest != 0;
  g_args.selftest_targets   = (p_targets != nullptr) ? p_targets : "";
  g_args.selftest_los       = (p_selftest_los != nullptr) ? p_selftest_los : "";
  g_args.sync_correction_enable = p_sync_correction != 0;
  g_args.sync_sto           = p_sync_sto != 0;
  g_args.sync_cfo           = p_sync_cfo != 0;
  g_args.sync_sfo           = p_sync_sfo != 0;
  g_args.sync_los           = p_sync_los != 0;
  g_args.nominal_los_range_m = (float)p_nominal_los_range;
  g_args.aoa_enable          = p_aoa_enable != 0;
  g_args.rx_array            = (p_rx_array != nullptr) ? p_rx_array : "";
  g_args.rx_array_boresight_deg = (float)p_aoa_boresight;
  g_args.aoa_estimator       = (p_aoa_est != nullptr) ? p_aoa_est : "beamscan";
  g_args.aoa_scan_step_deg   = (float)p_aoa_scan_step;
  g_args.aoa_min_snr_db      = (float)p_aoa_min_snr;
  g_args.aoa_cell_search_bins = (uint32_t)(p_aoa_search >= 0 ? p_aoa_search : 0);
  g_args.aoa_broadside_deg   = (float)p_aoa_broadside;
  g_args.aoa_selfcal         = p_aoa_selfcal != 0;
  g_args.track_enable        = p_track_enable != 0;
  g_args.track_model         = (p_track_model != nullptr) ? p_track_model : "cv";
  g_args.track_init_acc_var  = (float)p_track_init_acc;
  g_args.track_r_var_m2      = (float)p_track_r;
  g_args.track_rv_var_m2s2   = (float)p_track_rv;
  g_args.track_q_accel       = (float)p_track_q;
  g_args.track_gate_sigma    = (float)p_track_gate;
  g_args.track_init_vel_var_m2s2 = (float)p_track_vvar;
  g_args.track_max_coast     = (uint32_t)(p_track_max_coast > 0 ? p_track_max_coast : 1);
  g_args.track_q_adapt_enable = p_track_q_adapt != 0;
  g_args.track_nis_ewma_alpha = (float)p_track_nis_alpha;
  g_args.track_q_mult_max     = (float)p_track_q_mult_max;
  g_args.track_q_adapt_window_sigma = (float)p_track_adapt_win;
  g_args.track_max_tracks    = (uint32_t)(p_track_max_tracks > 0 ? p_track_max_tracks : 1);
  g_args.track_confirm_m     = (uint32_t)(p_track_confirm_m > 0 ? p_track_confirm_m : 0); // 0 = auto
  g_args.track_confirm_n     = (uint32_t)(p_track_confirm_n > 0 ? p_track_confirm_n : 1);
  g_args.track_confirm_target_pfa = (float)p_track_confirm_pfa;
  g_args.track_assoc_gate_sigma = (float)p_track_assoc_gate;
  g_args.track_harmonic_reject   = p_track_harm_reject != 0;
  g_args.track_harmonic_range_m  = (float)p_track_harm_range;
  g_args.track_flicker_reject      = p_track_flicker_reject != 0;
  g_args.track_flicker_max_db      = (float)p_track_flicker_max_db;
  g_args.track_flicker_min_updates = (uint32_t)(p_track_flicker_min_upd >= 0 ? p_track_flicker_min_upd : 0);
  g_args.track_flicker_ewma_alpha  = (float)p_track_flicker_alpha;
  g_args.track_assoc_gate_max_sr_m   = (float)p_track_assoc_max_sr;
  g_args.track_assoc_gate_max_sv_mps = (float)p_track_assoc_max_sv;
  g_args.cfar_guard         = (uint32_t)p_cfar_guard;
  g_args.cfar_train         = (uint32_t)p_cfar_train;
  g_args.cfar_per_column    = p_cfar_per_column != 0;
  g_args.cfar_per_row       = p_cfar_per_row != 0;
  g_args.subbin_interp      = p_subbin_interp != 0;
  g_args.gating_reject      = p_gating_reject != 0;
  g_args.gating_max_offsets = (uint32_t)(p_gating_max_off > 0 ? p_gating_max_off : 1);
  g_args.gating_min_rel     = (float)p_gating_min_rel;
  g_args.gating_tol_bins    = (uint32_t)(p_gating_tol >= 0 ? p_gating_tol : 0);
  g_args.gating_snr_margin  = (float)p_gating_snr_margin;
  g_args.cfar_pfa           = (float)p_cfar_pfa; // <=0 = auto (see defs_nr_UE_ISAC.h)
  g_args.cfar_target_fa_per_cpi = (float)p_cfar_target_fa;
  g_args.zero_doppler_guard = (uint32_t)p_zdg;
  g_args.zero_range_guard   = (uint32_t)p_zrg;
  g_args.nms_range_bins     = (uint32_t)p_nms_r;
  g_args.nms_doppler_bins   = (uint32_t)p_nms_d;
  g_args.max_detections     = (uint32_t)(p_maxdet > 0 ? p_maxdet : 1);
  g_args.conj_image_reject  = p_conj_reject != 0;
  g_args.conj_image_guard   = (uint32_t)(p_conj_guard >= 0 ? p_conj_guard : 0);
  g_args.harmonic_reject    = p_harm_reject != 0;
  g_args.harmonic_guard     = (uint32_t)(p_harm_guard >= 0 ? p_harm_guard : 0);
  g_args.harmonic_max_k     = (uint32_t)(p_harm_maxk >= 2 ? p_harm_maxk : 2);
  g_args.harmonic_tol       = (float)p_harm_tol;
  g_args.harmonic_snr_margin = (float)p_harm_snr_margin;
  g_args.harmonic_pos_reject     = p_harm_pos_reject != 0;
  g_args.harmonic_pos_chi2       = (float)p_harm_pos_chi2;
  g_args.harmonic_pos_range_tol_m = (float)p_harm_pos_range_tol;
  g_args.det_quality_adapt      = p_dq_adapt != 0;
  g_args.det_quality_cost_ratio = (float)p_dq_cost;
  g_args.adaptive_clutter_guard = p_adapt_guard != 0;
  g_args.max_range_m         = (float)p_max_range;
  g_args.far_harmonic_reject = p_far_harm_reject != 0;
  g_args.far_harmonic_far_m  = (float)p_far_harm_far;
  g_args.far_harmonic_near_m = (float)p_far_harm_near;
  g_args.cfar_fa_adapt_enable   = p_cfar_fa_adapt != 0;
  g_args.cfar_fa_min            = (float)p_cfar_fa_min;
  g_args.cfar_fa_max            = (float)p_cfar_fa_max;
  g_args.cfar_fa_adapt_rate     = (float)p_cfar_fa_rate;
  g_args.cfar_fa_target_min_det = (uint32_t)(p_cfar_fa_tmin >= 0 ? p_cfar_fa_tmin : 0);
  g_args.cfar_fa_target_max_det = (uint32_t)(p_cfar_fa_tmax > 0 ? p_cfar_fa_tmax : 1);
  g_args.cpi_quality_gate      = p_cpi_qgate != 0;
  g_args.cpi_quality_max_ratio = (float)p_cpi_qratio;
  g_args.cpi_quality_ema_alpha = (float)p_cpi_qalpha;
  g_args.clutter_removal    = (p_clutter != nullptr) ? p_clutter : "mean";
  g_args.eca_delay_max_m    = (float)p_eca_delay_max;
  g_args.eca_doppler_max_mps = (float)p_eca_dopp_max;
  g_args.detector           = (p_detector != nullptr) ? p_detector : "fft";
  g_args.mf_per_row_norm    = p_mf_per_row_norm != 0;
  g_args.clean_deconv       = p_clean_deconv != 0;
  g_args.clean_occ_aware    = p_clean_occ_aware != 0;
  g_args.subslot_symbols    = (uint32_t)(p_subslot_symbols > 0 ? p_subslot_symbols : 0);
  g_args.subslot_min_re     = (uint32_t)(p_subslot_min_re > 0 ? p_subslot_min_re : 0);
  g_args.subslot_min_snr_db = (float)p_subslot_min_snr_db;
  g_args.clean_max_components     = (uint32_t)(p_clean_max_comp > 0 ? p_clean_max_comp : 0); // 0 = auto
  g_args.clean_max_components_cap = (uint32_t)(p_clean_max_comp_cap > 0 ? p_clean_max_comp_cap : 1);
  g_args.clean_loop_gain    = (float)p_clean_loop_gain;
  g_args.clean_stop_db      = (float)p_clean_stop_db;
  g_args.clean_restore_bins = (uint32_t)(p_clean_restore >= 0 ? p_clean_restore : 0);
  g_args.range_window       = (p_range_win != nullptr) ? p_range_win : "hann";
  g_args.range_window_sidelobe_db = (float)p_range_window_sll;
  g_args.range_whiten       = p_range_whiten != 0;
  g_args.doppler_nudft      = p_doppler_nudft != 0;
  g_args.slow_time_complete = p_mc_enable != 0;
  g_args.mc_rank            = (uint32_t)(p_mc_rank > 0 ? p_mc_rank : 0); // 0 = auto
  g_args.mc_rank_max        = (uint32_t)(p_mc_rank_max > 0 ? p_mc_rank_max : 1);
  g_args.mc_iters           = (uint32_t)(p_mc_iters > 0 ? p_mc_iters : 1);
  g_args.mc_power_iters     = (uint32_t)(p_mc_power >= 0 ? p_mc_power : 0);
  g_args.doppler_sparse             = p_sd_enable != 0;
  g_args.doppler_sparse_iters       = (uint32_t)(p_sd_iters > 0 ? p_sd_iters : 1);
  g_args.doppler_sparse_lambda_scale = (float)p_sd_lambda_scale;
  g_args.doppler_sparse_peak_ratio  = (float)p_sd_peak_ratio;
  g_args.doppler_sparse_lambda_min  = (float)p_sd_lambda_min;
  g_args.doppler_sparse_lambda_max  = (float)p_sd_lambda_max;
  g_args.doppler_sparse_adapt_rate  = (float)p_sd_adapt_rate;
  g_args.doppler_sparse_target_min_det = (uint32_t)(p_sd_target_min >= 0 ? p_sd_target_min : 0);
  g_args.doppler_sparse_target_max_det = (uint32_t)(p_sd_target_max > 0 ? p_sd_target_max : 1);
  g_args.doppler_sparse_harmonic_ratio = (float)p_sd_harmonic_ratio;
  g_args.out_path           = (p_out_path != nullptr) ? p_out_path : "/tmp/oaiue_sensing";
  g_args.rx_id              = (p_rx_id != nullptr) ? p_rx_id : "rx1";
  g_args.rx_pos_x           = (float)p_rx_x;
  g_args.rx_pos_y           = (float)p_rx_y;
  g_args.illuminator_id     = (p_illum_id != nullptr) ? p_illum_id : "gnb1";
  g_args.tx_pos_x           = (float)p_tx_x;
  g_args.tx_pos_y           = (float)p_tx_y;
  g_args.report_path        = (p_report != nullptr) ? p_report : "";
  g_args.report_endpoint    = (p_endpoint != nullptr) ? p_endpoint : "";

  // Resolve the array geometry up front so the RT taps have a fixed antenna count to extract, and so
  // a malformed rx_array is a start-up log line rather than a silently single-antenna run.
  g_aoa_antennas = 0;
  if (g_args.aoa_enable) {
    aoa_array_t probe;
    // The carrier frequency isn't known until the first CFR arrives; parse against a nominal value
    // purely to validate the spec and count elements. The engine re-parses with the real fc.
    if (parse_rx_array(g_args.rx_array, g_args.rx_array_boresight_deg, 3.5e9, probe, /*quiet=*/true)) {
      g_aoa_antennas = probe.size();
      LOG_I(PHY, "SENSING: AoA enabled, %u receive elements, estimator=%s selfcal=%d\n", g_aoa_antennas,
            g_args.aoa_estimator.c_str(), (int)g_args.aoa_selfcal);
    } else {
      LOG_W(PHY, "SENSING: aoa_enable set but rx_array ('%s') is unusable; AoA disabled\n",
            g_args.rx_array.c_str());
      g_args.aoa_enable = false;
    }
  }

  g_engine.reset(new sensing_engine(g_args, /*max_prb=*/ISAC_MAX_PRB));
  g_enabled.store(true);

  LOG_I(PHY,
        "SENSING: enabled sources=%s (mask=0x%x, primary=%s) cpi_slots=%u interpolate=%d capture=%d rx_id=%s "
        "illum=%s report_path='%s' endpoint='%s'\n",
        nr_isac_sources_to_ref_type(g_args.sources_mask).c_str(), g_args.sources_mask,
        nr_isac_source_to_ref_type(g_args.source), g_args.cpi_slots, (int)g_args.interpolate,
        (int)g_args.capture_enable, g_args.rx_id.c_str(), g_args.illuminator_id.c_str(), g_args.report_path.c_str(),
        g_args.report_endpoint.c_str());
}

extern "C" void nr_isac_start(void)
{
  if (!g_enabled.load() || !g_engine || g_started.exchange(true)) {
    return;
  }
  g_engine->start();
}

extern "C" void nr_isac_stop(void)
{
  if (!g_enabled.load() || !g_engine) {
    return;
  }
  g_engine->stop();
  g_started.store(false);
}

extern "C" int nr_isac_enabled(void)
{
  return g_enabled.load(std::memory_order_relaxed) ? 1 : 0;
}

extern "C" int nr_isac_source(void)
{
  return (int)g_args.source;
}

extern "C" int nr_isac_source_enabled(int source)
{
  if (!g_enabled.load(std::memory_order_relaxed) || source < 0 || source >= NR_ISAC_SRC_COUNT) {
    return 0;
  }
  return (g_args.sources_mask & (1u << source)) ? 1 : 0;
}

extern "C" uint32_t nr_isac_subslot_config(uint32_t* min_re, float* min_snr_db)
{
  if (!g_enabled.load(std::memory_order_relaxed)) {
    return 0;
  }
  if (min_re != nullptr) {
    *min_re = g_args.subslot_min_re;
  }
  if (min_snr_db != nullptr) {
    *min_snr_db = g_args.subslot_min_snr_db;
  }
  return g_args.subslot_symbols;
}

extern "C" void nr_isac_submit_cfr(uint32_t                 slot_idx,
                                   int                      source,
                                   const nr_isac_carrier_t* carrier,
                                   const float*             h,
                                   const uint32_t*          k_abs,
                                   const uint32_t*          l_sym,
                                   uint32_t                 nof_re,
                                   float                    noise_var)
{
  nr_isac_submit_cfr_at(slot_idx, 0.0f, source, carrier, h, k_abs, l_sym, nof_re, noise_var);
}

extern "C" void nr_isac_submit_cfr_at(uint32_t                 slot_idx,
                                      float                    slot_frac,
                                      int                      source,
                                      const nr_isac_carrier_t* carrier,
                                      const float*             h,
                                      const uint32_t*          k_abs,
                                      const uint32_t*          l_sym,
                                      uint32_t                 nof_re,
                                      float                    noise_var)
{
  nr_isac_submit_cfr_multi(slot_idx, slot_frac, source, carrier, h, 1, nof_re, k_abs, l_sym, nof_re, noise_var);
}

extern "C" void nr_isac_submit_cfr_multi(uint32_t                 slot_idx,
                                         float                    slot_frac,
                                         int                      source,
                                         const nr_isac_carrier_t* carrier,
                                         const float*             h,
                                         uint32_t                 nof_ant,
                                         uint32_t                 ant_stride_re,
                                         const uint32_t*          k_abs,
                                         const uint32_t*          l_sym,
                                         uint32_t                 nof_re,
                                         float                    noise_var)
{
  if (!g_enabled.load(std::memory_order_relaxed) || !g_engine || carrier == nullptr || h == nullptr || nof_re == 0) {
    return;
  }
  if (source < 0 || source >= NR_ISAC_SRC_COUNT) {
    source = (int)g_args.source;
  }
  if (nof_ant == 0) {
    nof_ant = 1;
  }
  if (ant_stride_re < nof_re) {
    ant_stride_re = nof_re;
  }

  // Convert the interleaved float CFR into icf_t, PACKING the strided per-antenna slices into the
  // contiguous antenna-major layout sensing_slot_t expects. thread_local so RT callers never contend
  // or allocate after the first slot of each producer thread.
  static thread_local std::vector<icf_t> cfr;
  const size_t total = (size_t)nof_ant * nof_re;
  if (cfr.size() < total) {
    cfr.resize(total);
  }
  for (uint32_t a = 0; a < nof_ant; a++) {
    const float* src = h + (size_t)2 * a * ant_stride_re;
    icf_t*       dst = cfr.data() + (size_t)a * nof_re;
    for (uint32_t i = 0; i < nof_re; i++) {
      dst[i] = icf_t(src[2 * i], src[2 * i + 1]);
    }
  }

  g_engine->submit(slot_idx, slot_frac, (nr_isac_source_t)source, *carrier, cfr.data(), nof_ant, k_abs, l_sym, nof_re,
                   noise_var);
}

extern "C" uint32_t nr_isac_aoa_antennas(void)
{
  if (!g_enabled.load(std::memory_order_relaxed) || !g_args.aoa_enable) {
    return 0;
  }
  return g_aoa_antennas;
}
