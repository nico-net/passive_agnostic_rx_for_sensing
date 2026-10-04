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

/* Receiver-facing CFR export adapter. No sensing algorithms run here. */
#include "nr_isac.h"
#include "nr_sense_api.h"
#include "nr_passive_cfg_epoch.h"
#include <algorithm>
#include <atomic>
#include <climits>
#include <cstring>
#include <string>
#include <vector>
extern "C" {
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
}

namespace {
// One full 275-PRB, 14-symbol carrier. Conversion storage is bounded and per producer.
constexpr uint32_t max_re = 275 * 12 * 14;
std::atomic<unsigned> active_sources{0};
unsigned configured_sources;
bool initialized, configured, reconf;
nr_sense_cfg_t cfg{};
std::string records_path, cfr_path, endpoint, rx_id, rx_array;
thread_local uint16_t capture_k[max_re];
thread_local uint8_t capture_l[max_re];

paramdef_t parameter(const char *name, int type)
{
  paramdef_t p{};
  std::strncpy(p.optname, name, sizeof(p.optname) - 1);
  p.type = type;
  return p;
}
paramdef_t integer(const char *name, int *ptr, int value)
{
  auto p = parameter(name, TYPE_INT);
  p.iptr = ptr; p.defintval = value;
  return p;
}
paramdef_t string(const char *name, char **ptr, const char *value = "")
{
  auto p = parameter(name, TYPE_STRING);
  p.strptr = ptr; p.defstrval = value;
  return p;
}

// Complete compatibility vocabulary of the retired adapter. Config helpers still accept
// file and command-line values, but none of these values affect receiver or export behaviour.
const struct { const char *name; int type; } legacy_keys[] = {
  {"source", TYPE_STRING},
  {"cpi_slots", TYPE_INT},
  {"interpolate", TYPE_INT},
  {"capture", TYPE_INT},
  {"selftest", TYPE_INT},
  {"selftest_targets", TYPE_STRING},
  {"selftest_los", TYPE_STRING},
  {"sync_correction", TYPE_INT},
  {"sync_sto", TYPE_INT},
  {"sync_cfo", TYPE_INT},
  {"sync_sfo", TYPE_INT},
  {"sync_los", TYPE_INT},
  {"nominal_los_range_m", TYPE_DOUBLE},
  {"cfar_guard", TYPE_INT},
  {"cfar_train", TYPE_INT},
  {"cfar_per_column", TYPE_INT},
  {"cfar_per_row", TYPE_INT},
  {"subbin_interp", TYPE_INT},
  {"gating_reject", TYPE_INT},
  {"gating_max_offsets", TYPE_INT},
  {"gating_min_rel", TYPE_DOUBLE},
  {"gating_tol_bins", TYPE_INT},
  {"gating_snr_margin", TYPE_DOUBLE},
  {"cfar_pfa", TYPE_DOUBLE},
  {"cfar_target_fa_per_cpi", TYPE_DOUBLE},
  {"zero_doppler_guard", TYPE_INT},
  {"zero_range_guard", TYPE_INT},
  {"dealias_taper_bins", TYPE_INT},
  {"nms_range_bins", TYPE_INT},
  {"nms_doppler_bins", TYPE_INT},
  {"max_detections", TYPE_INT},
  {"conj_image_reject", TYPE_INT},
  {"conj_image_guard", TYPE_INT},
  {"harmonic_reject", TYPE_INT},
  {"harmonic_guard", TYPE_INT},
  {"harmonic_max_k", TYPE_INT},
  {"harmonic_tol", TYPE_DOUBLE},
  {"harmonic_snr_margin", TYPE_DOUBLE},
  {"harmonic_pos_reject", TYPE_INT},
  {"harmonic_pos_chi2", TYPE_DOUBLE},
  {"harmonic_pos_range_tol_m", TYPE_DOUBLE},
  {"det_quality_adapt", TYPE_INT},
  {"det_quality_cost_ratio", TYPE_DOUBLE},
  {"adaptive_clutter_guard", TYPE_INT},
  {"max_range_m", TYPE_DOUBLE},
  {"far_harmonic_reject", TYPE_INT},
  {"far_harmonic_far_m", TYPE_DOUBLE},
  {"far_harmonic_near_m", TYPE_DOUBLE},
  {"cfar_fa_adapt_enable", TYPE_INT},
  {"cfar_fa_min", TYPE_DOUBLE},
  {"cfar_fa_max", TYPE_DOUBLE},
  {"cfar_fa_adapt_rate", TYPE_DOUBLE},
  {"cfar_fa_target_min_det", TYPE_INT},
  {"cfar_fa_target_max_det", TYPE_INT},
  {"cpi_quality_gate", TYPE_INT},
  {"cpi_quality_max_ratio", TYPE_DOUBLE},
  {"cpi_quality_ema_alpha", TYPE_DOUBLE},
  {"clutter_removal", TYPE_STRING},
  {"eca_delay_max_m", TYPE_DOUBLE},
  {"eca_doppler_max_mps", TYPE_DOUBLE},
  {"detector", TYPE_STRING},
  {"mf_per_row_norm", TYPE_INT},
  {"clean_deconv", TYPE_INT},
  {"subslot_symbols", TYPE_INT},
  {"subslot_min_re", TYPE_INT},
  {"subslot_min_snr_db", TYPE_DOUBLE},
  {"clean_occ_aware", TYPE_INT},
  {"clean_max_components", TYPE_INT},
  {"clean_max_components_cap", TYPE_INT},
  {"clean_loop_gain", TYPE_DOUBLE},
  {"clean_stop_db", TYPE_DOUBLE},
  {"clean_restore_bins", TYPE_INT},
  {"range_window", TYPE_STRING},
  {"range_window_sidelobe_db", TYPE_DOUBLE},
  {"range_whiten", TYPE_INT},
  {"doppler_nudft", TYPE_INT},
  {"slow_time_complete", TYPE_INT},
  {"mc_rank", TYPE_INT},
  {"mc_rank_max", TYPE_INT},
  {"mc_iters", TYPE_INT},
  {"mc_power_iters", TYPE_INT},
  {"doppler_sparse", TYPE_INT},
  {"doppler_sparse_iters", TYPE_INT},
  {"doppler_sparse_lambda_scale", TYPE_DOUBLE},
  {"doppler_sparse_peak_ratio", TYPE_DOUBLE},
  {"doppler_sparse_harmonic_ratio", TYPE_DOUBLE},
  {"doppler_sparse_lambda_min", TYPE_DOUBLE},
  {"doppler_sparse_lambda_max", TYPE_DOUBLE},
  {"doppler_sparse_adapt_rate", TYPE_DOUBLE},
  {"doppler_sparse_target_min_det", TYPE_INT},
  {"doppler_sparse_target_max_det", TYPE_INT},
  {"track_enable", TYPE_INT},
  {"track_model", TYPE_STRING},
  {"track_init_acc_var", TYPE_DOUBLE},
  {"track_r_var_m2", TYPE_DOUBLE},
  {"track_rv_var_m2s2", TYPE_DOUBLE},
  {"track_q_accel", TYPE_DOUBLE},
  {"track_gate_sigma", TYPE_DOUBLE},
  {"track_init_vel_var", TYPE_DOUBLE},
  {"track_max_coast", TYPE_INT},
  {"track_q_adapt", TYPE_INT},
  {"track_nis_ewma_alpha", TYPE_DOUBLE},
  {"track_q_mult_max", TYPE_DOUBLE},
  {"track_q_adapt_window_sigma", TYPE_DOUBLE},
  {"track_max_tracks", TYPE_INT},
  {"track_confirm_m", TYPE_INT},
  {"track_confirm_n", TYPE_INT},
  {"track_confirm_target_pfa", TYPE_DOUBLE},
  {"track_assoc_gate_sigma", TYPE_DOUBLE},
  {"track_harmonic_reject", TYPE_INT},
  {"track_harmonic_range_m", TYPE_DOUBLE},
  {"track_flicker_reject", TYPE_INT},
  {"track_flicker_max_db", TYPE_DOUBLE},
  {"track_flicker_min_updates", TYPE_INT},
  {"track_flicker_ewma_alpha", TYPE_DOUBLE},
  {"track_assoc_gate_max_sr_m", TYPE_DOUBLE},
  {"track_assoc_gate_max_sv_mps", TYPE_DOUBLE},
  {"aoa_enable", TYPE_INT},
  {"rx_array_boresight_deg", TYPE_DOUBLE},
  {"aoa_estimator", TYPE_STRING},
  {"aoa_scan_step_deg", TYPE_DOUBLE},
  {"aoa_min_snr_db", TYPE_DOUBLE},
  {"aoa_cell_search_bins", TYPE_INT},
  {"aoa_broadside_deg", TYPE_DOUBLE},
  {"aoa_selfcal", TYPE_INT},
  {"out_path", TYPE_STRING},
  {"rx_pos_x", TYPE_DOUBLE},
  {"rx_pos_y", TYPE_DOUBLE},
  {"illuminator_id", TYPE_STRING},
  {"tx_pos_x", TYPE_DOUBLE},
  {"tx_pos_y", TYPE_DOUBLE},
  {"report_path", TYPE_STRING},
  {"report_endpoint", TYPE_STRING},
};

std::string json_string(const std::string &s)
{
  if (s.empty()) return "null";
  const char hex[] = "0123456789abcdef";
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
    else out += c;
  }
  return out + "\"";
}
unsigned source_mask(const char *s)
{
  unsigned mask = 0;
  std::string input = s ? s : "";
  size_t pos = 0;
  while (pos < input.size()) {
    size_t end = input.find(',', pos);
    std::string token = input.substr(pos, end == std::string::npos ? end : end - pos);
    pos = end == std::string::npos ? input.size() : end + 1;
    size_t first = token.find_first_not_of(" \t");
    if (first == std::string::npos) continue;
    token = token.substr(first, token.find_last_not_of(" \t") - first + 1);
    if (token == "all") mask |= (1u << NR_ISAC_SRC_COUNT) - 1;
    else if (token == "csirs" || token == "csi_rs") mask |= 1u << NR_ISAC_SRC_CSI_RS;
    else if (token == "ssb") mask |= 1u << NR_ISAC_SRC_SSB;
    else if (token == "pdsch_dmrs") mask |= (1u << NR_ISAC_SRC_PDSCH_DMRS) | (1u << NR_ISAC_SRC_PDSCH_DMRS_BLIND);
    else if (token == "pdsch_dmrs_blind") mask |= 1u << NR_ISAC_SRC_PDSCH_DMRS_BLIND;
    else if (token == "pdsch_data") mask |= 1u << NR_ISAC_SRC_PDSCH_DATA;
    else if (token == "pusch_dmrs") mask |= 1u << NR_ISAC_SRC_PUSCH_DMRS;
    else if (token == "pusch_data") mask |= 1u << NR_ISAC_SRC_PUSCH_DATA;
    else LOG_W(PHY, "SENSING: unknown source '%s' ignored\n", token.c_str());
  }
  return mask;
}
constexpr nr_sense_src_t source_map[] = {
  NR_SENSE_SRC_CSIRS, NR_SENSE_SRC_PDSCH_DMRS, NR_SENSE_SRC_PDSCH_DATA,
  NR_SENSE_SRC_PDSCH_DMRS, NR_SENSE_SRC_PUSCH_DMRS, NR_SENSE_SRC_PUSCH_DATA, NR_SENSE_SRC_SSB
};
}

extern "C" void nr_isac_init(void)
{
  if (initialized) return;
  initialized = true;
  int enable = 0, frames = 64, re = max_re;
  char *sources = nullptr, *rec = nullptr, *cfr = nullptr, *ep = nullptr, *id = nullptr, *array = nullptr;
  std::vector<paramdef_t> params = {
    integer("enable", &enable, 0), string("sources", &sources, "all"),
    string("cfr_path", &cfr), string("records_path", &rec), string("endpoint", &ep),
    integer("cfr_ring_frames", &frames, 64), integer("cfr_max_re", &re, max_re),
    string("rx_id", &id), string("rx_array", &array)
  };
  params[0].paramflags = PARAMFLAG_BOOL;
  const size_t legacy_begin = params.size();
  for (const auto &key : legacy_keys) {
    auto p = parameter(key.name, key.type);
    if (key.type == TYPE_STRING) p.defstrval = "";
    // Null value pointers let the config module own and free each discarded value.
    params.push_back(p);
  }
  if (config_get(config_get_if(), params.data(), params.size(), "sensing") < 0) return;
  std::string ignored;
  for (size_t i = legacy_begin; i < params.size(); ++i) {
    if (!config_isparamset(params.data(), i)) continue;
    if (!ignored.empty()) ignored += ",";
    ignored += params[i].optname;
  }
  if (!ignored.empty()) LOG_W(PHY, "SENSING: ignored legacy algorithm keys: %s\n", ignored.c_str());
  if (!enable) return;
  if (frames < 1 || frames > 65536 || re < 1 || re > (int)max_re) {
    LOG_E(PHY, "SENSING: invalid cfr_ring_frames (1..65536) or cfr_max_re (1..%u)\n", max_re);
    return;
  }
  records_path = rec ? rec : ""; cfr_path = cfr ? cfr : ""; endpoint = ep ? ep : "";
  rx_id = id ? id : ""; rx_array = array ? array : "";
  cfg = {records_path.empty() ? nullptr : records_path.c_str(),
         cfr_path.empty() ? nullptr : cfr_path.c_str(), endpoint.empty() ? nullptr : endpoint.c_str(),
         (uint32_t)frames, (uint32_t)re, 1024};
  configured_sources = source_mask(sources);
  reconf = nr_cfg_reconf_enabled(); // Resolve pthread_once before any producer runs.
  configured = true;
}

extern "C" void nr_isac_start(void)
{
  if (!configured || nr_sense_enabled()) return;
  if (nr_sense_open(&cfg) != 0) {
    LOG_E(PHY, "SENSING: cannot open export API\n");
    return;
  }
  const auto stamp = reconf ? nr_cfg_epoch_snapshot() : nr_cfg_epoch_snapshot_t{};
  // Task 2 config metadata; RF metadata and per-observation records belong to Task 3.
  const std::string session = "{\"schema\":2,\"type\":\"session\",\"reconf\":" + std::string(reconf ? "true" : "false")
    + ",\"epoch\":" + (reconf ? std::to_string(stamp.epoch) : "null")
    + ",\"identity_gen\":" + (reconf ? std::to_string(stamp.identity_gen) : "null")
    + ",\"rx_id\":" + json_string(rx_id) + ",\"rx_array\":" + json_string(rx_array) + "}";
  nr_sense_push_record(session.c_str());
  active_sources.store(configured_sources, std::memory_order_release);
}

extern "C" void nr_isac_stop(void)
{
  active_sources.store(0, std::memory_order_release);
  nr_sense_close();
  initialized = configured = false;
}
extern "C" int nr_isac_enabled(void) { return active_sources.load(std::memory_order_acquire) != 0; }
extern "C" int nr_isac_source(void)
{
  unsigned mask = active_sources.load(std::memory_order_acquire);
  return mask ? __builtin_ctz(mask) : NR_ISAC_SRC_CSI_RS;
}
extern "C" int nr_isac_source_enabled(int source)
{
  unsigned mask = active_sources.load(std::memory_order_acquire);
  return source >= 0 && source < NR_ISAC_SRC_COUNT && (mask & (1u << source));
}
extern "C" uint32_t nr_isac_subslot_config(uint32_t *min_re, float *min_snr_db)
{
  if (min_re) *min_re = 0;
  if (min_snr_db) *min_snr_db = 0;
  return 0;
}
extern "C" void nr_isac_submit_cfr_multi(uint32_t slot_idx, float slot_frac, int source,
    const nr_isac_carrier_t *carrier, const float *h, uint32_t nof_ant, uint32_t ant_stride_re,
    const uint32_t *k_abs, const uint32_t *l_sym, uint32_t nof_re, float noise_var)
{
  if (!nr_isac_source_enabled(source)) return;
  if (!h || !k_abs || !l_sym || nof_ant < 1 || nof_ant > 8 || nof_re < 1 || nof_re > INT_MAX ||
      ant_stride_re < nof_re || ant_stride_re > INT_MAX) return;
  (void)carrier; // Carrier metadata is emitted by Task 3.
  const uint32_t keep = std::min(nof_re, cfg.cfr_max_re);
  for (uint32_t i = 0; i < keep; ++i) {
    if (k_abs[i] > UINT16_MAX || l_sym[i] > UINT8_MAX) return;
    capture_k[i] = k_abs[i]; capture_l[i] = l_sym[i];
  }
  // Coherent epoch/identity pair published with the authoritative snapshot, no mutex on RX.
  const uint64_t stamp = reconf ? nr_cfg_epoch_cfr_stamp() : 0;
  nr_sense_push_cfr(source_map[source], nof_ant, h, ant_stride_re, nof_re, capture_k, capture_l,
                    slot_idx, slot_frac, noise_var, stamp >> 32, (uint32_t)stamp, nullptr);
}
extern "C" void nr_isac_submit_cfr_at(uint32_t slot_idx, float slot_frac, int source,
    const nr_isac_carrier_t *carrier, const float *h, const uint32_t *k_abs, const uint32_t *l_sym,
    uint32_t nof_re, float noise_var)
{
  nr_isac_submit_cfr_multi(slot_idx, slot_frac, source, carrier, h, 1, nof_re, k_abs, l_sym, nof_re, noise_var);
}
extern "C" void nr_isac_submit_cfr(uint32_t slot_idx, int source, const nr_isac_carrier_t *carrier,
    const float *h, const uint32_t *k_abs, const uint32_t *l_sym, uint32_t nof_re, float noise_var)
{
  nr_isac_submit_cfr_at(slot_idx, 0, source, carrier, h, k_abs, l_sym, nof_re, noise_var);
}
