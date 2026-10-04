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
#include "nr_sense_records.h"
#include "nr_passive_cfg_epoch.h"
#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
extern "C" {
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
}

namespace {
// One full 275-PRB, 14-symbol carrier.
constexpr uint32_t max_re = 275 * 12 * 14;
/* Masks use the receiver enum; source_map preserves the CFR wire IDs. */
std::atomic<unsigned> active_sources{0};
unsigned configured_sources;
bool initialized, configured, reconf;
uint32_t receiver_antennas = 1, cfr_antennas;
uint32_t subslot_symbols, subslot_min_re;
float subslot_min_snr_db;
nr_sense_cfg_t cfg{};
std::string records_path, cfr_path, endpoint, rx_id, rx_array;

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
    if (token == "csi_rs") mask |= 1u << NR_ISAC_SRC_CSI_RS;
    else if (token == "ssb") mask |= 1u << NR_ISAC_SRC_SSB;
    else if (token == "pdsch_dmrs") mask |= 1u << NR_ISAC_SRC_PDSCH_DMRS;
    else if (token == "pdsch_dmrs_blind") mask |= 1u << NR_ISAC_SRC_PDSCH_DMRS_BLIND;
    else if (token == "pdsch_data") mask |= 1u << NR_ISAC_SRC_PDSCH_DATA;
    else if (token == "pusch_dmrs") mask |= 1u << NR_ISAC_SRC_PUSCH_DMRS;
    else if (token == "pusch_data") mask |= 1u << NR_ISAC_SRC_PUSCH_DATA;
    else if (token == "prach") mask |= 1u << NR_ISAC_SRC_PRACH;
    else if (token == "srs") mask |= 1u << NR_ISAC_SRC_SRS;
    else LOG_W(PHY, "SENSING: unknown source '%s' ignored\n", token.c_str());
  }
  return mask;
}
constexpr nr_sense_src_t source_map[] = {
  NR_SENSE_SRC_CSIRS, NR_SENSE_SRC_PDSCH_DMRS, NR_SENSE_SRC_PDSCH_DATA,
  NR_SENSE_SRC_PDSCH_DMRS, NR_SENSE_SRC_PUSCH_DMRS, NR_SENSE_SRC_PUSCH_DATA, NR_SENSE_SRC_SSB,
  NR_SENSE_SRC_PRACH, NR_SENSE_SRC_SRS
};
static_assert(sizeof(source_map)/sizeof(source_map[0]) == NR_ISAC_SRC_COUNT);
}

extern "C" void nr_isac_init(void)
{
  if (initialized) return;
  initialized = true;
  // At 4 RX and 106 PRB the worst allowed RE cap still fits: 32 *
  // (48 + 3*46200 + 8*4*46200 + 3) + 1024*4096 + index arrays
  // = 55,944,160 bytes (53.35 MiB). cfr_ring_frames/cfr_max_re may override it.
  int enable = 0, frames = 32, re = max_re, antennas = 0, sub_syms = 0, sub_re = 600;
  double sub_snr = 0;
  char *sources = nullptr, *legacy_source = nullptr, *rec = nullptr, *cfr = nullptr, *ep = nullptr, *id = nullptr, *array = nullptr;
  std::vector<paramdef_t> params = {
    integer("enable", &enable, 0), string("sources", &sources), string("source", &legacy_source, "csi_rs"),
    string("cfr_path", &cfr), string("records_path", &rec), string("endpoint", &ep),
    integer("cfr_ring_frames", &frames, 32), integer("cfr_max_re", &re, max_re), integer("cfr_antennas", &antennas, 0),
    integer("subslot_symbols", &sub_syms, 0), integer("subslot_min_re", &sub_re, 600),
    string("rx_id", &id), string("rx_array", &array)
  };
  auto snr = parameter("subslot_min_snr_db", TYPE_DOUBLE); snr.dblptr = &sub_snr; snr.defdblval = 0;
  params.push_back(snr);
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
  if (frames < 1 || frames > 65536 || re < 1 || re > (int)max_re || antennas < 0 || antennas > 8) {
    LOG_E(PHY, "SENSING: invalid CFR ring, RE, or antenna limit\n");
    return;
  }
  records_path = rec ? rec : ""; cfr_path = cfr ? cfr : ""; endpoint = ep ? ep : "";
  rx_id = id ? id : ""; rx_array = array ? array : "";
  cfg = {records_path.empty() ? nullptr : records_path.c_str(),
         cfr_path.empty() ? nullptr : cfr_path.c_str(), endpoint.empty() ? nullptr : endpoint.c_str(),
         (uint32_t)frames, (uint32_t)re, 1024, 1};
  configured_sources = source_mask(sources);
  if (!configured_sources) {
    const std::string fallback = legacy_source ? legacy_source : "csi_rs";
    configured_sources = fallback == "pdsch_dmrs" ? 1u << NR_ISAC_SRC_PDSCH_DMRS
      : fallback == "pdsch_data" ? 1u << NR_ISAC_SRC_PDSCH_DATA : 1u << NR_ISAC_SRC_CSI_RS;
  }
  cfr_antennas = antennas;
  subslot_symbols = std::max(sub_syms,0); subslot_min_re = std::max(sub_re,0); subslot_min_snr_db = sub_snr;
  reconf = nr_cfg_reconf_enabled(); // Resolve pthread_once before any producer runs.
  configured = true;
}

extern "C" void nr_isac_start(void)
{
  if (!configured || nr_sense_enabled()) return;
  cfg.cfr_max_ant = cfr_antennas ? std::min(cfr_antennas,receiver_antennas) : receiver_antennas;
  if (!cfg.records_path && !cfg.cfr_path && !cfg.endpoint)
    LOG_W(PHY, "SENSING: no records_path/cfr_path is set; CFR frames will be counted as no_sink\n");
  if (nr_sense_open(&cfg) != 0) {
    LOG_E(PHY, "SENSING: cannot open export API\n");
    return;
  }
  nr_sense_set_session_identity(rx_id.empty() ? nullptr : rx_id.c_str(), rx_array.empty() ? nullptr : rx_array.c_str());
  nr_sense_emit_session(nullptr, NAN);
  active_sources.store(configured_sources, std::memory_order_release);
}

extern "C" void nr_isac_set_rx_antennas(uint32_t antennas)
{
  if (antennas >= 1 && antennas <= 8) receiver_antennas = std::max(receiver_antennas,antennas);
}

extern "C" void nr_isac_stop(void)
{
  active_sources.store(0, std::memory_order_release);
  nr_sense_close();
  initialized = configured = false;
  receiver_antennas = 1;
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
extern "C" int nr_isac_prach_enabled(void)
{
  return nr_isac_source_enabled(NR_ISAC_SRC_PRACH);
}
extern "C" uint32_t nr_isac_subslot_config(uint32_t *min_re, float *min_snr_db)
{
  if (!nr_isac_enabled()) {
    if (min_re) *min_re = 0;
    if (min_snr_db) *min_snr_db = 0;
    return 0;
  }
  if (min_re) *min_re = subslot_min_re;
  if (min_snr_db) *min_snr_db = subslot_min_snr_db;
  return subslot_symbols;
}
extern "C" void nr_isac_submit_cfr_multi_meta(uint32_t slot_idx, float slot_frac, int source,
    const nr_isac_carrier_t *carrier, const float *h, uint32_t nof_ant, uint32_t ant_stride_re,
    const uint32_t *k_abs, const uint32_t *l_sym, uint32_t nof_re, float noise_var,
    const nr_sense_rs_meta_t *detail)
{
  if (!nr_isac_source_enabled(source)) return;
  if (!h || !k_abs || !l_sym || nof_ant < 1 || nof_ant > 8 || nof_re < 1 || nof_re > INT_MAX ||
      ant_stride_re < nof_re || ant_stride_re > INT_MAX) { nr_sense_reject_cfr(); return; }
  // Coherent epoch/identity pair published with the authoritative snapshot, no mutex on RX.
  const uint64_t stamp = reconf ? nr_cfg_epoch_cfr_stamp() : 0;
  const int dedicated = reconf ? int(nr_cfg_epoch_dedicated_current()) : -1;
  bool truncated=false;
  const int n_export_ant=std::min(nof_ant,cfg.cfr_max_ant);
  const uint64_t frame_id=nr_sense_push_cfr_u32(source_map[source], n_export_ant, h, ant_stride_re, nof_re, k_abs, l_sym,
                    slot_idx, slot_frac, noise_var, stamp >> 32, (uint32_t)stamp, &truncated);
  nr_sense_rs_meta_t m{};
  m.rnti=0; m.start_rb=-1; m.nb_rb=-1; m.n_re=std::min(nof_re,cfg.cfr_max_re);
  m.n_ant=n_export_ant; m.ports=-1; m.ssb_index=-1; m.k_ssb=-1;
  m.noise_var=noise_var; m.slot_frac=slot_frac; m.abs_slot=slot_idx;
  m.frame=carrier && carrier->slots_per_frame ? (slot_idx/carrier->slots_per_frame)%1024 : -1;
  m.slot=carrier && carrier->slots_per_frame ? slot_idx%carrier->slots_per_frame : -1;
  m.pci=carrier?carrier->pci:-1; m.rx_ts=-1;
  m.stamp_valid=true;
  m.epoch=reconf ? int(stamp>>32) : -1;
  m.identity_gen=reconf ? int(uint32_t(stamp)) : -1;
  m.dedicated_epoch=dedicated;
  m.ta_est_samples=-1; m.transform_precoding=-1; m.dmrs_type=-1; m.dmrs_add_pos=-1; m.dmrs_n_scid=-1;
  m.prach_preamble=-1; m.prach_energy=NAN; m.prach_delay=NAN;
  m.srs_comb=m.srs_offset=m.srs_cs=m.srs_group=m.srs_period=-1;
  for (uint32_t r=0;r<(uint32_t)m.n_re;r++) {
    if (l_sym[r]<32) m.l_sym_mask|=1u<<l_sym[r];
    if (k_abs[r]<=UINT16_MAX && (m.start_rb<0 || int(k_abs[r]/12)<m.start_rb)) m.start_rb=k_abs[r]/12;
  }
  if (detail) {
    m.rnti=detail->rnti; m.start_rb=detail->start_rb; m.nb_rb=detail->nb_rb;
    m.ports=detail->ports; m.port_mask=detail->port_mask;
    m.ssb_index=detail->ssb_index; m.k_ssb=detail->k_ssb;
    m.csirs_resource=detail->csirs_resource; m.ta_est_samples=detail->ta_est_samples;
    m.ta_applied_samples=detail->ta_applied_samples;
    m.ta_applied_valid=detail->ta_applied_valid;
    m.transform_precoding=detail->transform_precoding; m.dmrs_type=detail->dmrs_type;
    m.dmrs_add_pos=detail->dmrs_add_pos; m.dmrs_n_scid=detail->dmrs_n_scid;
  }
  m.truncated=truncated;
  nr_sense_emit_rs(source_map[source],&m,frame_id);
}
extern "C" void nr_isac_submit_cfr_multi(uint32_t slot_idx, float slot_frac, int source,
    const nr_isac_carrier_t *carrier, const float *h, uint32_t nof_ant, uint32_t ant_stride_re,
    const uint32_t *k_abs, const uint32_t *l_sym, uint32_t nof_re, float noise_var)
{
  nr_isac_submit_cfr_multi_meta(slot_idx,slot_frac,source,carrier,h,nof_ant,ant_stride_re,k_abs,l_sym,nof_re,noise_var,nullptr);
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
