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
  int    p_cpi_slots = 256;
  int    p_cfar_guard = 4, p_cfar_train = 8;
  double p_cfar_pfa = 1e-3;
  int    p_zdg = 3, p_zrg = 2, p_nms_r = 3, p_nms_d = 3, p_maxdet = 32;
  double p_rx_x = 0.0, p_rx_y = 0.0, p_tx_x = 0.0, p_tx_y = 0.0;
  char*  p_source     = nullptr;
  char*  p_sources    = nullptr;
  char*  p_targets    = nullptr;
  char*  p_selftest_los = nullptr;
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
      mk_int("cfar_guard", "CA-CFAR guard cells per side", 0, &p_cfar_guard, 4),
      mk_int("cfar_train", "CA-CFAR training cells per side", 0, &p_cfar_train, 8),
      mk_dbl("cfar_pfa", "CA-CFAR false-alarm probability", &p_cfar_pfa, 1e-3),
      mk_int("zero_doppler_guard", "Doppler notch half-width (bins)", 0, &p_zdg, 3),
      mk_int("zero_range_guard", "range notch half-width (bins)", 0, &p_zrg, 2),
      mk_int("nms_range_bins", "NMS radius in range bins", 0, &p_nms_r, 3),
      mk_int("nms_doppler_bins", "NMS radius in Doppler bins", 0, &p_nms_d, 3),
      mk_int("max_detections", "cap on detections per CPI", 0, &p_maxdet, 32),
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
  g_args.cfar_guard         = (uint32_t)p_cfar_guard;
  g_args.cfar_train         = (uint32_t)p_cfar_train;
  g_args.cfar_pfa           = (float)p_cfar_pfa;
  g_args.zero_doppler_guard = (uint32_t)p_zdg;
  g_args.zero_range_guard   = (uint32_t)p_zrg;
  g_args.nms_range_bins     = (uint32_t)p_nms_r;
  g_args.nms_doppler_bins   = (uint32_t)p_nms_d;
  g_args.max_detections     = (uint32_t)(p_maxdet > 0 ? p_maxdet : 1);
  g_args.out_path           = (p_out_path != nullptr) ? p_out_path : "/tmp/oaiue_sensing";
  g_args.rx_id              = (p_rx_id != nullptr) ? p_rx_id : "rx1";
  g_args.rx_pos_x           = (float)p_rx_x;
  g_args.rx_pos_y           = (float)p_rx_y;
  g_args.illuminator_id     = (p_illum_id != nullptr) ? p_illum_id : "gnb1";
  g_args.tx_pos_x           = (float)p_tx_x;
  g_args.tx_pos_y           = (float)p_tx_y;
  g_args.report_path        = (p_report != nullptr) ? p_report : "";
  g_args.report_endpoint    = (p_endpoint != nullptr) ? p_endpoint : "";

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

extern "C" void nr_isac_submit_cfr(uint32_t                 slot_idx,
                                   int                      source,
                                   const nr_isac_carrier_t* carrier,
                                   const float*             h,
                                   const uint32_t*          k_abs,
                                   const uint32_t*          l_sym,
                                   uint32_t                 nof_re)
{
  if (!g_enabled.load(std::memory_order_relaxed) || !g_engine || carrier == nullptr || h == nullptr || nof_re == 0) {
    return;
  }
  if (source < 0 || source >= NR_ISAC_SRC_COUNT) {
    source = (int)g_args.source;
  }

  // Convert the interleaved float CFR into icf_t. thread_local so RT callers never contend or allocate
  // after the first slot of each producer thread.
  static thread_local std::vector<icf_t> cfr;
  if (cfr.size() < nof_re) {
    cfr.resize(nof_re);
  }
  for (uint32_t i = 0; i < nof_re; i++) {
    cfr[i] = icf_t(h[2 * i], h[2 * i + 1]);
  }

  g_engine->submit(slot_idx, (nr_isac_source_t)source, *carrier, cfr.data(), k_abs, l_sym, nof_re);
}
