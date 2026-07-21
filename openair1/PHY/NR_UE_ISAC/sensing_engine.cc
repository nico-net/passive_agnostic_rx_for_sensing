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

#include "sensing_engine.h"
#include "detection_report.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>

extern "C" {
#include "common/utils/LOG/log.h"
}

#ifdef ENABLE_ZEROMQ
#include <zmq.h>
#endif

namespace nr_isac {

// Number of pre-allocated snapshots cycling between the free and ready queues.
static constexpr uint32_t SENSING_SLOT_POOL_SIZE = 64;

// Native comb of a row = smallest gap between consecutive occupied (real-sample) subcarriers.
// 12 for CSI-RS-per-RB, 2 for PDSCH DM-RS, 1 for full-allocation/merged rows. 1 (no clipping) if a
// row has fewer than two occupied columns.
static uint32_t row_native_comb(const uint8_t* mask, uint32_t nof_subc)
{
  uint32_t comb = 0;
  int      prev = -1;
  for (uint32_t c = 0; c < nof_subc; c++) {
    if (mask[c]) {
      if (prev >= 0) {
        const uint32_t d = c - (uint32_t)prev;
        if (comb == 0 || d < comb) {
          comb = d;
        }
      }
      prev = (int)c;
    }
  }
  return (comb > 0) ? comb : 1;
}

sensing_engine::sensing_engine(const nr_isac_args_t& args_, uint32_t max_prb_) : args(args_), max_prb(max_prb_)
{
  // A CPI must contain at least one slow-time sample
  if (args.cpi_slots == 0) {
    args.cpi_slots = 1;
  }

  // Upper bound on the number of REs extracted from a single slot (pre-reserve so the RT thread never
  // reallocates): subcarriers/PRB x symbols/slot x max PRB.
  const uint32_t max_re = (max_prb > 0 ? max_prb : ISAC_MAX_PRB) * ISAC_NRE * ISAC_NSYMB;

  // Pre-allocate the snapshot pool and reserve their buffers so submit() never allocates.
  slot_pool.resize(SENSING_SLOT_POOL_SIZE);
  for (sensing_slot_t& s : slot_pool) {
    s.h.reserve(max_re);
    s.k_abs.reserve(max_re);
    s.l_sym.reserve(max_re);
    free_q.push(&s);
  }

  // Resolve the DetectionReport JSON-lines path: explicit report_path wins, otherwise derive it from
  // the output prefix. If neither is set, report emission stays disabled.
  if (!args.report_path.empty()) {
    report_path = args.report_path;
  } else if (!args.out_path.empty()) {
    report_path = args.out_path + "_reports.jsonl";
  }
  report_endpoint = args.report_endpoint;

  rd.reset(new range_doppler(args));
}

sensing_engine::~sensing_engine()
{
  stop();
}

void sensing_engine::start()
{
  if (running.exchange(true)) {
    return; // Already running
  }
  engine_thread = std::thread(&sensing_engine::run_thread, this);
}

void sensing_engine::stop()
{
  if (!running.exchange(false)) {
    return; // Not running
  }
  ready_q.push(nullptr); // shutdown sentinel
  if (engine_thread.joinable()) {
    engine_thread.join();
  }
}

void sensing_engine::submit(uint32_t                 slot_idx,
                            nr_isac_source_t         source,
                            const nr_isac_carrier_t& carrier,
                            const icf_t*              h,
                            const uint32_t*          k_abs,
                            const uint32_t*          l,
                            uint32_t                 nof_re)
{
  if (!running.load(std::memory_order_relaxed) || h == nullptr || nof_re == 0) {
    return;
  }

  sensing_slot_t* s = nullptr;
  if (!free_q.try_pop(s) || s == nullptr) {
    dropped_slots.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  s->slot_idx = slot_idx;
  s->source   = source;
  s->carrier  = carrier;
  s->nof_re   = nof_re;
  s->h.assign(h, h + nof_re);
  s->k_abs.assign(k_abs, k_abs + nof_re);
  s->l_sym.assign(l, l + nof_re);

  // Comb spacing = smallest positive gap between consecutive subcarriers (2 for DM-RS-only, 1 when
  // reconstructed/interpolated data REs fill every subcarrier). Robust to the RE ordering.
  uint32_t comb = 0;
  for (uint32_t i = 1; i < nof_re; i++) {
    if (k_abs[i] > k_abs[i - 1]) {
      const uint32_t d = k_abs[i] - k_abs[i - 1];
      if (comb == 0 || d < comb) {
        comb = d;
      }
    }
  }
  s->comb_spacing = (comb > 0) ? comb : 1;
  s->period_slots = 1;

  ready_q.push(s);
}

void sensing_engine::run_thread()
{
  LOG_I(PHY, "SENSING: engine thread started (cpi_slots=%u sources_mask=0x%x primary=%d)\n", args.cpi_slots,
        args.sources_mask, (int)args.source);

  // Open the optional live DetectionReport bus here so the (non-thread-safe) socket lives entirely on
  // the engine thread.
  report_bus_open();

  while (running.load(std::memory_order_relaxed)) {
    sensing_slot_t* s = ready_q.wait_pop();
    if (s == nullptr) {
      break; // Shutdown sentinel
    }

    const uint64_t n = processed_slots.fetch_add(1, std::memory_order_relaxed) + 1;

    if (n % 50 == 1) {
      LOG_I(PHY,
            "SENSING: reference occurrence #%lu slot_idx=%u nof_re=%u comb=%u period_slots=%u dropped=%lu\n",
            (unsigned long)n, s->slot_idx, s->nof_re, s->comb_spacing, s->period_slots,
            (unsigned long)dropped_slots.load(std::memory_order_relaxed));
    }

    if (s->nof_re > 0) {
      accumulate_cpi(*s);
    }

    // Recycle the snapshot (clear keeps the reserved capacity).
    s->h.clear();
    s->k_abs.clear();
    s->l_sym.clear();
    free_q.push(s);
  }

  report_bus_close();

  LOG_I(PHY, "SENSING: engine thread stopped (processed=%lu dropped=%lu)\n",
        (unsigned long)processed_slots.load(std::memory_order_relaxed),
        (unsigned long)dropped_slots.load(std::memory_order_relaxed));
}

void sensing_engine::accumulate_cpi(const sensing_slot_t& s)
{
  const uint32_t band_sc = s.carrier.nof_prb * ISAC_NRE; // full per-subcarrier grid width (column = abs subcarrier)
  if (band_sc == 0) {
    return;
  }

  // (Re)initialise the fused full-band grid on the first row, or when the carrier geometry changes.
  // The grid spans the whole carrier lattice so any comb from any source lands on it; unoccupied
  // subcarriers are gap-filled at CPI close. Column = absolute subcarrier (relative to CRB0).
  const bool geom_ok = (cpi_row > 0) && (band_sc == nof_subc) && (s.carrier.nof_prb == cpi_carrier.nof_prb) &&
                       (s.carrier.pci == cpi_carrier.pci) && (s.carrier.scs_hz == cpi_carrier.scs_hz);
  if (!geom_ok) {
    if (cpi_row > 0) {
      LOG_I(PHY, "SENSING: grid geometry changed (subc %u -> %u, pci %u -> %u); resetting CPI\n", nof_subc, band_sc,
            cpi_carrier.pci, s.carrier.pci);
    }
    nof_subc      = band_sc;
    cpi_grid_comb = 1;
    cpi_carrier   = s.carrier;
    h_cpi.assign((size_t)args.cpi_slots * nof_subc, icf_t(0.0f, 0.0f));
    occ_all.assign((size_t)args.cpi_slots * nof_subc, 0);
    cpi_row_time.assign(args.cpi_slots, 0.0);
    cpi_row_comb.assign(args.cpi_slots, 1);
    row_comb_uniform.assign(args.cpi_slots, 1);
    for (int i = 0; i < NR_ISAC_SRC_COUNT; i++) {
      src_occ[i] = 0;
    }
    cpi_row       = 0;
    cpi_slot_span = 0;
    cpi_prev_slot = s.slot_idx;
  }

  // Unwrapped slow-time position (in slots) of this submission relative to the CPI time origin.
  uint32_t this_span;
  if (cpi_row == 0) {
    this_span     = 0; // first submission anchors the CPI time origin
    cpi_prev_slot = s.slot_idx;
  } else {
    const uint32_t slots_per_frame = (s.carrier.slots_per_frame > 0) ? s.carrier.slots_per_frame : 10;
    const uint32_t wrap            = slots_per_frame * 1024;
    const uint32_t delta           = (wrap + s.slot_idx - cpi_prev_slot) % wrap;
    this_span                      = (uint32_t)(cpi_slot_span + delta);
    cpi_prev_slot                  = s.slot_idx;
  }

  // Absolute-slot-indexed rows: fold this submission into the current row if it shares that row's real
  // slot (a second source densifying the same slot), otherwise open a new slow-time row. Rows are
  // reused across CPIs, so a freshly opened row is cleared before first use.
  const bool merge = (cpi_row > 0) && ((uint64_t)this_span == cpi_slot_span);
  uint32_t   r;
  if (merge) {
    r = cpi_row - 1;
  } else {
    r               = cpi_row;
    cpi_slot_span   = this_span;
    cpi_row_time[r] = (double)this_span;
    std::fill(&h_cpi[(size_t)r * nof_subc], &h_cpi[(size_t)r * nof_subc] + nof_subc, icf_t(0.0f, 0.0f));
    std::memset(&occ_all[(size_t)r * nof_subc], 0, nof_subc);
    cpi_row++;
  }

  if ((uint32_t)s.source < (uint32_t)NR_ISAC_SRC_COUNT) {
    src_occ[s.source]++;
  }
  cpi_comb_spacing = (s.comb_spacing > 0) ? s.comb_spacing : cpi_comb_spacing;

  // Place this submission's samples onto their absolute-subcarrier columns; last-write-wins on any
  // per-column conflict (two sources hitting the same subcarrier — rare, they occupy disjoint REs by
  // design). Frequency gap-fill is deferred to CPI close so a merged row is interpolated only once.
  icf_t*   row  = &h_cpi[(size_t)r * nof_subc];
  uint8_t* mask = &occ_all[(size_t)r * nof_subc];
  for (uint32_t i = 0; i < s.nof_re; i++) {
    const uint32_t k = s.k_abs[i];
    if (k < nof_subc) {
      row[k]  = s.h[i];
      mask[k] = 1;
    }
  }

  if (cpi_row >= args.cpi_slots) {
    // Per-row native comb (for DSP de-aliasing AND Phase 1 STO tracking below), computed from the
    // real-sample occupancy BEFORE any correction or gap-fill.
    for (uint32_t rr = 0; rr < cpi_row; rr++) {
      cpi_row_comb[rr] = row_native_comb(&occ_all[(size_t)rr * nof_subc], nof_subc);
    }

    // Phase 1 (ota_sync_passive_ue.md): per-row LOS CIR peak tracking + fine-STO correction on the
    // RAW grid, before Stage-4b interpolation -- interpolation should operate on already-timing-
    // corrected data. See docs/NR_UE_ISAC_sync_gap_analysis.md for why this must run here.
    last_sto_fit = sto_tracker.process(h_cpi.data(), occ_all.data(), cpi_row, nof_subc, cpi_row_comb.data(),
                                        cpi_row_time.data(), cpi_carrier);

    // Phase 2 (ota_sync_passive_ue.md): residual-CFO fit + per-row CPE de-rotation, reusing Phase 1's
    // per-row LOS-tap estimates (no second CIR pass). Also runs before Stage-4b interpolation, and
    // commutes with the STO correction above (uniform per-row rotation vs. STO's per-subcarrier ramp).
    last_cfo_fit = cfo_tracker.process(h_cpi.data(), occ_all.data(), nof_subc, sto_tracker.last_row_estimates());

    // Stage 4b (part 1): gap-fill every accumulated row now that same-slot merges + STO correction
    // are done.
    for (uint32_t rr = 0; rr < cpi_row; rr++) {
      if (args.interpolate) {
        interp_freq_row(&h_cpi[(size_t)rr * nof_subc], &occ_all[(size_t)rr * nof_subc], nof_subc);
      }
    }

    // Mean slow-time period (slots) across the CPI; falls back to the reported per-source period.
    cpi_period_slots = (args.cpi_slots > 1 && cpi_slot_span > 0)
                           ? (uint32_t)((cpi_slot_span + (args.cpi_slots - 1) / 2) / (args.cpi_slots - 1))
                           : ((s.period_slots > 0) ? s.period_slots : 1);
    if (cpi_period_slots == 0) {
      cpi_period_slots = 1;
    }
    // Stage 4b (part 2): resample the non-uniform-in-time rows onto a uniform slow-time grid.
    if (args.interpolate) {
      resample_slow_time();
    } else {
      h_cpi_uniform    = h_cpi;
      row_comb_uniform = cpi_row_comb; // rows map 1:1 when not resampling
    }
    process_cpi();
    cpi_row = 0;
  }
}

void sensing_engine::interp_freq_row(icf_t* row, const uint8_t* mask, uint32_t nof_subc) const
{
  // Locate the first/last occupied subcarrier
  int first = -1, last = -1;
  for (uint32_t c = 0; c < nof_subc; c++) {
    if (mask[c]) {
      if (first < 0) {
        first = (int)c;
      }
      last = (int)c;
    }
  }
  if (first < 0) {
    return; // nothing occupied in this row
  }

  // Hold at the band edges (flat extrapolation beyond the occupied span)
  for (int c = 0; c < first; c++) {
    row[c] = row[first];
  }
  for (int c = last + 1; c < (int)nof_subc; c++) {
    row[c] = row[last];
  }

  // Linear-interpolate the interior gaps between consecutive occupied columns
  int prev = first;
  for (int c = first + 1; c <= last; c++) {
    if (!mask[c]) {
      continue;
    }
    if (c > prev + 1) {
      const icf_t va   = row[prev];
      const icf_t vb   = row[c];
      const int  span = c - prev;
      for (int gg = prev + 1; gg < c; gg++) {
        const float f = (float)(gg - prev) / (float)span;
        row[gg]       = icf_t((1.0f - f) * va.real() + f * vb.real(), (1.0f - f) * va.imag() + f * vb.imag());
      }
    }
    prev = c;
  }
}

void sensing_engine::resample_slow_time()
{
  const uint32_t N = args.cpi_slots;
  h_cpi_uniform.assign((size_t)N * nof_subc, icf_t(0.0f, 0.0f));
  row_comb_uniform.assign(N, 1);
  if (N < 2) {
    h_cpi_uniform    = h_cpi;
    row_comb_uniform = cpi_row_comb;
    return;
  }
  const double span = cpi_row_time[N - 1];
  if (span <= 0.0) {
    h_cpi_uniform    = h_cpi; // degenerate (all occurrences at the same slot): pass through
    row_comb_uniform = cpi_row_comb;
    return;
  }

  // Rows are monotonically increasing in time; advance a running source pointer as the uniform grid
  // point tg increases, and linearly interpolate each subcarrier between the two bracketing rows.
  uint32_t r = 0;
  for (uint32_t gg = 0; gg < N; gg++) {
    const double tg = (double)gg * span / (double)(N - 1);
    while (r < N - 2 && cpi_row_time[r + 1] < tg) {
      r++;
    }
    const double t0   = cpi_row_time[r];
    const double t1   = cpi_row_time[r + 1];
    const float  frac = (t1 > t0) ? (float)((tg - t0) / (t1 - t0)) : 0.0f;
    const icf_t*  a    = &h_cpi[(size_t)r * nof_subc];
    const icf_t*  b    = &h_cpi[(size_t)(r + 1) * nof_subc];
    icf_t*        dst  = &h_cpi_uniform[(size_t)gg * nof_subc];
    for (uint32_t c = 0; c < nof_subc; c++) {
      dst[c] = icf_t((1.0f - frac) * a[c].real() + frac * b[c].real(), (1.0f - frac) * a[c].imag() + frac * b[c].imag());
    }
    // A blended row carries the coarser (larger comb) of its two source rows, so its de-alias window is
    // the conservative one — a blend of a comb-12 and a comb-2 row still contains the comb-12 structure.
    row_comb_uniform[gg] = std::max(cpi_row_comb[r], cpi_row_comb[r + 1]);
  }
}

void sensing_engine::process_cpi()
{
  // System-clock stand-in for the CPI start time (ns since the Unix epoch). NOTE: NOT GPSDO/PPS-aligned
  // — carries the host wall-clock offset/jitter; only meaningful for single-receiver bench testing and
  // the central node's file-replay path. Real multi-receiver fusion needs a disciplined shared clock.
  cpi_start_time_utc_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

  cpi_count++;

  // Stage 3: range-Doppler DSP on the (Stage-4b) uniformly-resampled CPI matrix. The fused grid is
  // per-subcarrier (column spacing = 1 subcarrier), so the range axis is scaled with cpi_grid_comb (1),
  // independent of any individual source's native comb.
  rd->process(h_cpi_uniform.data(), args.cpi_slots, nof_subc, cpi_grid_comb, cpi_carrier,
              (float)cpi_period_slots, row_comb_uniform.data(), rvm, detections);

  const sensing_detection_t* top = nullptr;
  for (const sensing_detection_t& d : detections) {
    if (top == nullptr || d.snr_db > top->snr_db) {
      top = &d;
    }
  }

  if (top != nullptr) {
    LOG_I(PHY,
          "SENSING: CPI #%u fc=%.1f MHz subc=%u T_slot=%u occ[csi=%lu dmrs=%lu data=%lu] range[res=%.2f max=%.0f]m "
          "vel[res=%.3f max=%.1f]m/s detections=%zu top: range=%.1f m vel=%.2f m/s snr=%.1f dB\n",
          cpi_count, cpi_carrier.dl_center_hz / 1e6, nof_subc, cpi_period_slots,
          (unsigned long)src_occ[NR_ISAC_SRC_CSI_RS], (unsigned long)src_occ[NR_ISAC_SRC_PDSCH_DMRS],
          (unsigned long)src_occ[NR_ISAC_SRC_PDSCH_DATA], rvm.range_res_m, rvm.range_max_m, rvm.vel_res_mps,
          rvm.vel_max_mps, detections.size(), top->range_m, top->vel_mps, top->snr_db);
  } else {
    LOG_I(PHY,
          "SENSING: CPI #%u fc=%.1f MHz subc=%u occ[csi=%lu dmrs=%lu data=%lu] range_res=%.2f m vel_res=%.3f m/s "
          "detections=0\n",
          cpi_count, cpi_carrier.dl_center_hz / 1e6, nof_subc, (unsigned long)src_occ[NR_ISAC_SRC_CSI_RS],
          (unsigned long)src_occ[NR_ISAC_SRC_PDSCH_DMRS], (unsigned long)src_occ[NR_ISAC_SRC_PDSCH_DATA],
          rvm.range_res_m, rvm.vel_res_mps);
  }

  write_outputs();
}

void sensing_engine::write_outputs()
{
  // Legacy CSV / RVM-raster outputs (gated on the output prefix).
  if (!args.out_path.empty()) {
    if (!detections.empty()) {
      const std::string det_path = args.out_path + "_detections.csv";
      std::ofstream     f(det_path, std::ios::app);
      if (f.is_open()) {
        for (const sensing_detection_t& d : detections) {
          f << cpi_count << ',' << d.range_bin << ',' << d.doppler_bin << ',' << d.range_m << ',' << d.vel_mps << ','
            << d.snr_db << '\n';
        }
      }
    }

    if (args.capture_enable && !rvm.power.empty()) {
      const std::string rvm_path = args.out_path + "_rvm_" + std::to_string(cpi_count) + ".f32";
      std::ofstream     f(rvm_path, std::ios::binary);
      if (f.is_open()) {
        f.write(reinterpret_cast<const char*>(rvm.power.data()), (std::streamsize)(rvm.power.size() * sizeof(float)));
      }
    }
  }

  // DetectionReport JSON-lines (central-node detection bus). Independent of the CSV/RVM path above.
  write_report_json();
}

void sensing_engine::write_report_json()
{
  if (report_path.empty() && zmq_pub == nullptr) {
    return;
  }

  // CPI duration = cpi_slots x slot duration. A 10 ms radio frame holds slots_per_frame slots.
  const uint32_t slots_per_frame = (cpi_carrier.slots_per_frame > 0) ? cpi_carrier.slots_per_frame : 10;
  const int64_t  slot_dur_ns     = (slots_per_frame > 0) ? (int64_t)(10000000LL / slots_per_frame) : 0;

  detection_report_t rep;
  rep.rx_id                 = args.rx_id;
  rep.illuminator_id        = args.illuminator_id;
  rep.pci                   = cpi_carrier.pci;
  rep.ref_type              = nr_isac_sources_to_ref_type(args.sources_mask);
  rep.tx_pos_x              = args.tx_pos_x;
  rep.tx_pos_y              = args.tx_pos_y;
  rep.rx_pos_x              = args.rx_pos_x;
  rep.rx_pos_y              = args.rx_pos_y;
  rep.cpi_start_time_utc_ns = cpi_start_time_utc_ns;
  rep.cpi_duration_ns       = slot_dur_ns * (int64_t)args.cpi_slots;
  rep.fc_hz                 = (double)cpi_carrier.dl_center_hz;
  rep.rvm                   = &rvm;
  rep.detections            = &detections;
  rep.include_rvm_blob      = args.capture_enable;

  const std::string line = build_detection_report_json(rep);

  if (!report_path.empty()) {
    std::ofstream f(report_path, std::ios::app);
    if (f.is_open()) {
      f << line << '\n';
    } else {
      LOG_W(PHY, "SENSING: could not open DetectionReport path '%s'\n", report_path.c_str());
    }
  }

#ifdef ENABLE_ZEROMQ
  if (zmq_pub != nullptr) {
    const int rc = zmq_send(zmq_pub, line.data(), line.size(), 0);
    if (rc < 0) {
      LOG_W(PHY, "SENSING: ZeroMQ DetectionReport send failed: %s\n", zmq_strerror(zmq_errno()));
    }
  }
#endif
}

void sensing_engine::report_bus_open()
{
  if (report_endpoint.empty()) {
    return;
  }
#ifdef ENABLE_ZEROMQ
  zmq_ctx = zmq_ctx_new();
  if (zmq_ctx == nullptr) {
    LOG_E(PHY, "SENSING: ZeroMQ context creation failed: %s\n", zmq_strerror(zmq_errno()));
    return;
  }
  zmq_pub = zmq_socket(zmq_ctx, ZMQ_PUB);
  if (zmq_pub == nullptr) {
    LOG_E(PHY, "SENSING: ZeroMQ PUB socket creation failed: %s\n", zmq_strerror(zmq_errno()));
    zmq_ctx_destroy(zmq_ctx);
    zmq_ctx = nullptr;
    return;
  }
  // Bind (not connect): the central node connects a SUB socket to us. A short linger keeps stop() from
  // blocking on undelivered frames.
  const int linger_ms = 100;
  zmq_setsockopt(zmq_pub, ZMQ_LINGER, &linger_ms, sizeof(linger_ms));
  if (zmq_bind(zmq_pub, report_endpoint.c_str()) != 0) {
    LOG_E(PHY, "SENSING: ZeroMQ PUB bind '%s' failed: %s\n", report_endpoint.c_str(), zmq_strerror(zmq_errno()));
    zmq_close(zmq_pub);
    zmq_pub = nullptr;
    zmq_ctx_destroy(zmq_ctx);
    zmq_ctx = nullptr;
    return;
  }
  LOG_I(PHY, "SENSING: DetectionReport ZeroMQ PUB bound to %s\n", report_endpoint.c_str());
#else
  LOG_W(PHY,
        "SENSING: sensing.report_endpoint set to '%s' but this build has no ZeroMQ support; the live "
        "DetectionReport bus is disabled\n",
        report_endpoint.c_str());
#endif
}

void sensing_engine::report_bus_close()
{
#ifdef ENABLE_ZEROMQ
  if (zmq_pub != nullptr) {
    zmq_close(zmq_pub);
    zmq_pub = nullptr;
  }
  if (zmq_ctx != nullptr) {
    zmq_ctx_destroy(zmq_ctx);
    zmq_ctx = nullptr;
  }
#endif
}

} // namespace nr_isac
