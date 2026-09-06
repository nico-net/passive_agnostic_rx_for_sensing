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
#include <cstdlib>
#include <fstream>

extern "C" {
#include "common/utils/LOG/log.h"
}

#ifdef ENABLE_ZEROMQ
#include <zmq.h>
#endif

namespace nr_isac {

// Number of pre-allocated snapshots cycling between the free and ready queues.
/* Sized to cover a CPI CLOSE, not the steady stream. The consumer is one thread doing
 * wait_pop -> accumulate_cpi() -> recycle, and the close (interpolation, clutter removal, range
 * IFFT, Doppler FFT, CFAR, NMS, AoA, tracking, serialisation) happens INSIDE accumulate_cpi() --
 * so for its whole duration nothing returns to free_q and every submission is dropped on the spot
 * (:155). Measured 2026-08-31 at 273 PRB / cpi_slots=128: drops arrive in BURSTS at CPI
 * boundaries, not steadily, and cost 63-78 % of all submissions -- which starves the slow-time
 * grid enough that the STO peak search finds zero valid rows and SFO never gets a fit.
 * 64 slots is ~32 ms of buffering at ~0.5 ms/slot; 512 is ~256 ms. Overridable so the trade can be
 * measured without a rebuild. THE REAL FIX is to close the CPI off the drain thread; this only
 * buys headroom, so keep watching `dropped=` rather than assuming it is solved. */
static uint32_t sensing_slot_pool_size()
{
  const char* e = getenv("ISAC_SLOT_POOL");
  const int   v = (e != nullptr) ? atoi(e) : 0;
  return (v >= 8 && v <= 8192) ? (uint32_t)v : 512u;
}

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
  // Receive-array AoA: fix the antenna count up front so the RT snapshot buffers can be reserved for
  // it (submit() must never allocate). Parsed here against a nominal fc purely for the element COUNT;
  // the real geometry is re-parsed at the first CPI, once the carrier frequency is known.
  aoa_ant_ = 0;
  if (args.aoa_enable) {
    aoa_array_t probe;
    if (parse_rx_array(args.rx_array, args.rx_array_boresight_deg, 3.5e9, probe, /*quiet=*/true)) {
      aoa_ant_ = probe.size();
    } else {
      args.aoa_enable = false;
    }
  }
  const uint32_t re_per_slot = (aoa_ant_ > 1) ? max_re * aoa_ant_ : max_re;

  slot_pool.resize(sensing_slot_pool_size());
  for (sensing_slot_t& s : slot_pool) {
    s.h.reserve(re_per_slot);
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
  if (args.track_enable) {
    bistatic_geometry_3d_t geom(vec3_t(args.tx_pos_x, args.tx_pos_y, args.tx_pos_z),
                                vec3_t(args.rx_pos_x, args.rx_pos_y, args.rx_pos_z));
    hierarchical_tracker_config_t hier_cfg;
    hier_tracker.reset(new hierarchical_tracker(geom, hier_cfg));
    tracker.reset(new multi_target_tracker(args));
  }
}

sensing_engine::~sensing_engine()
{
  stop();
}

/* slots_per_frame, DERIVED. nr_isac.cc never populates carrier.slots_per_frame, so every call site
 * fell back to a hardcoded 10 -- correct only at 15 kHz SCS. This deployment runs 30 kHz, where it
 * is 20, so the slot-index wrap used by the span accumulator was 10*1024 instead of 20*1024. Every
 * real wrap was then mis-corrected (measured: slot_idx stepping 8675 -> 6645 backwards), which
 * inflated cpi_slot_span ~190x: T_slot read 4356 slots against a true ~22, cpi_duration_ns read
 * 268 s for a CPI that really lasted 1.45 s, and vel_res/vel_max collapsed to 0 -- i.e. no usable
 * Doppler axis at all. 3GPP: slots per 10 ms frame = 10 * 2^mu = 10 * scs/15 kHz. */
static inline uint32_t isac_slots_per_frame(const nr_isac_carrier_t& c)
{
  if (c.slots_per_frame > 0) {
    return c.slots_per_frame;
  }
  if (c.scs_hz >= 15000.0) {
    return (uint32_t)llround(10.0 * (c.scs_hz / 15000.0));
  }
  return 10;
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
                            float                    slot_frac,
                            nr_isac_source_t         source,
                            const nr_isac_carrier_t& carrier,
                            const icf_t*              h,
                            uint32_t                 nof_ant,
                            const uint32_t*          k_abs,
                            const uint32_t*          l,
                            uint32_t                 nof_re,
                            float                    noise_var)
{
  if (!running.load(std::memory_order_relaxed) || h == nullptr || nof_re == 0) {
    return;
  }

  sensing_slot_t* s = nullptr;
  if (!free_q.try_pop(s) || s == nullptr) {
    dropped_slots.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  s->slot_idx  = slot_idx;
  s->slot_frac = (slot_frac >= 0.0f && slot_frac < 1.0f) ? slot_frac : 0.0f;
  s->source    = source;
  s->carrier   = carrier;
  s->nof_re    = nof_re;
  s->noise_var = noise_var;
  // Only carry the extra antennas when AoA is actually configured; a single-antenna receiver copies
  // exactly as much as it always did.
  s->nof_ant   = (aoa_ant_ > 1 && nof_ant > 1) ? std::min(nof_ant, aoa_ant_) : 1u;
  if (s->nof_ant > aoa_ant_seen_) {
    aoa_ant_seen_ = s->nof_ant;
  }
  s->h.assign(h, h + (size_t)s->nof_ant * nof_re);
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
    wsum_all.assign((size_t)args.cpi_slots * nof_subc, 0.0f);
    if (aoa_ant_ > 1) {
      h_cpi_ant.assign((size_t)aoa_ant_ * args.cpi_slots * nof_subc, icf_t(0.0f, 0.0f));
      // The array geometry needs the real carrier frequency, which is only known now.
      aoa_array_ready_ = parse_rx_array(args.rx_array, args.rx_array_boresight_deg,
                                        (double)s.carrier.dl_center_hz, aoa_array);
      aoa.reset(aoa_array_ready_ ? new aoa_estimator(args, aoa_array) : nullptr);
    }
    cpi_row_time.assign(args.cpi_slots, 0.0);
    cpi_row_comb.assign(args.cpi_slots, 1);
    cpi_row_illum.assign(args.cpi_slots, (uint8_t)NR_ISAC_ILLUM_DL);
    row_comb_uniform.assign(args.cpi_slots, 1);
    for (int i = 0; i < NR_ISAC_SRC_COUNT; i++) {
      src_occ[i] = 0;
    }
    cpi_row       = 0;
    cpi_slot_span     = 0;
    cpi_step_sum_     = 0.0;
    cpi_step_n_       = 0;
    cpi_step_rejected_ = 0;
    cpi_prev_slot = s.slot_idx;
  }

  // Advance the monotonic unwrapped slot counter (across CPI boundaries) for the true-dt measurement.
  {
    const uint32_t slots_per_frame = isac_slots_per_frame(s.carrier);
    const uint32_t wrap            = slots_per_frame * 1024;
    if (!abs_init_) {
      abs_init_     = true;
      abs_prev_raw_ = s.slot_idx;
    } else {
      /* SIGNED shortest-path delta. The unsigned `(wrap + a - b) % wrap` form is only correct while
       * submissions arrive in slot ORDER: an out-of-order arrival 2 slots early yields
       * (20480 + 998 - 1000) % 20480 = 20478, not -2. That never happened while the PDSCH decode ran
       * in-line, and happens constantly now that it is deferred to a CONCURRENT consumer pool. */
      int64_t d_run = (int64_t)s.slot_idx - (int64_t)abs_prev_raw_;
      if (d_run > (int64_t)wrap / 2) {
        d_run -= (int64_t)wrap;
      } else if (d_run < -(int64_t)wrap / 2) {
        d_run += (int64_t)wrap;
      }
      if (d_run > 0) {
        abs_slot_run_ += (uint64_t)d_run; // out-of-order arrivals must not rewind the run counter
        abs_prev_raw_ = s.slot_idx;
      }
      /* REFERENCE MUST NOT FOLLOW A REJECTED STEP. Rewinding abs_prev_raw_ to an early
       * out-of-order slot while abs_slot_run_ is held makes the NEXT in-order submission
       * measure its delta from that earlier slot, so the backwards step is added right
       * back on -- the rejection is undone one submission later. Keep the reference at the
       * highest slot seen. */
    }
  }

  // Unwrapped slow-time position (in slots) of this submission relative to the CPI time origin.
  // FRACTIONAL (sub-slot sampling): a submission's slow-time position is its integer slot delta plus
  // its within-slot offset, so several symbol groups from the SAME slot land on distinct rows at their
  // true times instead of collapsing onto one. slot_frac == 0 for every legacy caller, which
  // reproduces the previous integer behaviour exactly.
  double this_span;
  if (cpi_row == 0) {
    this_span       = 0.0; // first submission anchors the CPI time origin
    cpi_prev_slot   = s.slot_idx;
    cpi_prev_pos    = 0.0;
    cpi_anchor_abs_ = abs_slot_run_; // absolute anchor of this CPI, for the true inter-CPI dt
  } else {
    const uint32_t slots_per_frame = isac_slots_per_frame(s.carrier);
    const uint32_t wrap            = slots_per_frame * 1024;
    /* SIGNED shortest-path delta -- see the identical fix on abs_slot_run_ above. MEASURED
     * consequence of getting this wrong with a concurrent consumer pool: every out-of-order arrival
     * added ~20480 slots to cpi_slot_span, so a CPI of 128 rows whose true span is ~256 slots
     * reported 190,000, T_slot came out at ~1500 instead of ~2, and vel[max] collapsed to 0.1 m/s
     * on a receiver genuinely producing ~1000 rows/s. */
    int64_t d_cpi = (int64_t)s.slot_idx - (int64_t)cpi_prev_slot;
    if (d_cpi > (int64_t)wrap / 2) {
      d_cpi -= (int64_t)wrap;
    } else if (d_cpi < -(int64_t)wrap / 2) {
      d_cpi += (int64_t)wrap;
    }
    // Position of this submission relative to the previous one, carrying both fractions.
    /* BOUND THE FORWARD STEP. Rejecting only BACKWARDS steps (below) left the other half of
     * the same defect open: a stale or wrap-mis-detected submission looks like a jump of
     * hundreds of slots FORWARD, is accepted as real elapsed time, and inflates the CPI span
     * for good. MEASURED 2026-09-02 after the backwards-step fix: T_slot still bimodal, most
     * CPIs at 1.2-2.3 slots but a minority at 40-3227, and the source mix does NOT predict
     * which (two CPIs in one run, both ul_rows=74, read 2.14 and 1292.18) -- i.e. one bad
     * jump per bad CPI, not a systematic offset. The bound is the CPI's OWN running mean
     * accepted step, not a constant: legitimate spacing is set by this cell's scheduling and
     * varies run to run. FORWARD_STEP_MAX_FACTOR admits a genuine gap several times the
     * typical one (a real scheduling hole) while rejecting the 10^2-10^3 excursions. Held
     * until enough steps have been accepted to have an estimate at all. */
    if (cpi_step_n_ >= FORWARD_STEP_MIN_SAMPLES) {
      const double typical = cpi_step_sum_ / (double)cpi_step_n_;
      if ((double)d_cpi > FORWARD_STEP_MAX_FACTOR * std::max(typical, 1.0)) {
        d_cpi = 0; // treat as out-of-order/stale: contributes its CFR, does not move the clock
        cpi_step_rejected_++;
      }
    }
    this_span     = cpi_prev_pos + (double)d_cpi + ((double)s.slot_frac - prev_frac_);
    if (this_span < cpi_prev_pos) {
      /* Hold the span AND the reference. Advancing cpi_prev_slot to the rejected (earlier)
       * slot made the next in-order submission measure from it, double-counting the
       * backwards step; over 128 rows with DL/PUSCH/UL submitting from concurrent threads
       * that accumulated without bound. MEASURED 2026-09-02: T_slot 285478-437334 slots
       * against a true 1.0-3.7 on every run with mixed sources, i.e. no Doppler axis at
       * all, while the one single-producer run in the same batch read a correct 1.244. */
      this_span = cpi_prev_pos;
    } else {
      cpi_prev_slot = s.slot_idx;
      if (d_cpi > 0) {
        cpi_step_sum_ += (double)d_cpi;
        cpi_step_n_++;
      }
    }
  }
  prev_frac_   = (double)s.slot_frac;
  cpi_prev_pos = this_span;

  // Absolute-slot-indexed rows: fold this submission into the current row if it shares that row's real
  // slot (a second source densifying the same slot), otherwise open a new slow-time row. Rows are
  // reused across CPIs, so a freshly opened row is cleared before first use.
  const bool merge = (cpi_row > 0) && (std::fabs(this_span - cpi_slot_span) < 1e-6);
  uint32_t   r;
  if (merge) {
    r = cpi_row - 1;
  } else {
    r               = cpi_row;
    cpi_slot_span   = this_span;
    cpi_row_time[r] = this_span;
    std::fill(&h_cpi[(size_t)r * nof_subc], &h_cpi[(size_t)r * nof_subc] + nof_subc, icf_t(0.0f, 0.0f));
    std::memset(&occ_all[(size_t)r * nof_subc], 0, nof_subc);
    std::fill(&wsum_all[(size_t)r * nof_subc], &wsum_all[(size_t)r * nof_subc] + nof_subc, 0.0f);
    for (uint32_t a = 0; a < aoa_ant_ && !h_cpi_ant.empty(); a++) {
      icf_t* p = &h_cpi_ant[((size_t)a * args.cpi_slots + r) * nof_subc];
      std::fill(p, p + nof_subc, icf_t(0.0f, 0.0f));
    }
    cpi_row++;
  }

  if ((uint32_t)s.source < (uint32_t)NR_ISAC_SRC_COUNT) {
    src_occ[s.source]++;
    /* Stamp the row's illuminator. On a merge the row already has one: TDD says the two must agree,
     * so a disagreement is a real anomaly (a slot carrying both PDSCH and PUSCH) rather than
     * something to average -- keep the first and let the count below surface it. */
    if (r < cpi_row_illum.size()) {
      const uint8_t il = (uint8_t)nr_isac_source_illum(s.source);
      if (!merge) {
        cpi_row_illum[r] = il;
      } else if (cpi_row_illum[r] != il) {
        n_mixed_illum_rows_++;
      }
    }
  }
  cpi_comb_spacing = (s.comb_spacing > 0) ? s.comb_spacing : cpi_comb_spacing;

  // Place this submission's samples onto their absolute-subcarrier columns, fusing per-column
  // conflicts (two sources hitting the same subcarrier in this slot) by INVERSE-VARIANCE weighting
  // rather than last-write-wins: the estimate with lower noise power dominates. h_cpi holds the
  // running weighted mean ĥ = Σ(ĥ_i·w_i)/Σw_i and wsum_all its running Σw_i, updated incrementally so
  // the order of same-slot source submissions doesn't matter. w_i = 1/σ²_i; an unknown/zero noise_var
  // falls back to unit weight (equal weighting = the old behaviour). Frequency gap-fill is deferred to
  // CPI close so a merged row is interpolated only once.
  /* ---- RECEIVE-BRANCH COMBINING FOR THE SENSING GRID (ISAC_SENSE_COMB=1) --------------------
   * h_cpi -- the grid STO, SFO, CFO and range-Doppler all run on -- was built from s.h[i], i.e.
   * ANTENNA 0 ONLY. ISAC_RX_MRC_MODE combines branches for the PDSCH DECODER and never reaches
   * here, so on a rig where antenna 0 is not the strongest branch the whole sync stack runs on the
   * weak one: rows fail the fade gate, STO flywheels, and SFO's line fit never clears its R^2 gate.
   *
   * For the single dominant path STO/CFO actually track, h_a[k] = h_0[k]*s_a with s_a a constant
   * array phase across k, so co-phasing to antenna 0 and summing IS maximum-ratio combining: up to
   * 10*log10(A) = 6 dB at four branches. s_a is estimated from this submission's own REs, so it
   * costs one pass and carries no state.
   *
   * h_cpi_ant is deliberately NOT touched -- AoA needs the per-antenna phases this collapses. */
  static const bool sense_comb = [] {
    const char* e = getenv("ISAC_SENSE_COMB");
    return e != nullptr && atoi(e) != 0;
  }();
  constexpr uint32_t COMB_MAX = 8;
  icf_t    comb_w[COMB_MAX];
  uint32_t comb_n = 1;
  if (sense_comb && s.nof_ant > 1 && s.nof_re > 0) {
    comb_n    = std::min<uint32_t>(s.nof_ant, COMB_MAX);
    comb_w[0] = icf_t(1.0f, 0.0f);
    for (uint32_t a = 1; a < comb_n; a++) {
      icf_t acc(0.0f, 0.0f);
      for (uint32_t i = 0; i < s.nof_re; i++) {
        acc += s.h[(size_t)a * s.nof_re + i] * std::conj(s.h[i]);
      }
      const float m = std::abs(acc);
      /* No usable correlation with antenna 0 (dead branch, or a fade): drop it rather than let an
       * arbitrary phase fold that branch's noise in. Weight 0 contributes exactly nothing. */
      comb_w[a] = (m > 0.0f) ? (std::conj(acc) / m) : icf_t(0.0f, 0.0f);
    }
    float norm = 0.0f;
    for (uint32_t a = 0; a < comb_n; a++) {
      norm += std::abs(comb_w[a]);
    }
    const float g = (norm > 0.0f) ? (1.0f / norm) : 1.0f;
    for (uint32_t a = 0; a < comb_n; a++) {
      comb_w[a] *= g;
    }
  }

  const float w = (s.noise_var > 0.0f) ? (1.0f / s.noise_var) : 1.0f;
  icf_t*   row  = &h_cpi[(size_t)r * nof_subc];
  uint8_t* mask = &occ_all[(size_t)r * nof_subc];
  float*   wrow = &wsum_all[(size_t)r * nof_subc];
  for (uint32_t i = 0; i < s.nof_re; i++) {
    const uint32_t k = s.k_abs[i];
    if (k < nof_subc) {
      const float w_prev = wrow[k];
      const float w_new  = w_prev + w;
      // Running weighted mean: row[k] <- (row[k]*w_prev + h_i*w) / (w_prev + w).
      // First writer (w_prev==0) reduces to row[k] = h_i exactly.
      icf_t h_in = s.h[i];
      if (comb_n > 1) {
        h_in = icf_t(0.0f, 0.0f);
        for (uint32_t a = 0; a < comb_n; a++) {
          h_in += s.h[(size_t)a * s.nof_re + i] * comb_w[a];
        }
      }
      row[k]  = (row[k] * w_prev + h_in * w) * (1.0f / w_new);
      wrow[k] = w_new;
      mask[k] = 1;
      // Mirror onto the raw per-antenna grid with the SAME inverse-variance weighting, so a fused
      // multi-source row stays consistent between the two grids. Antenna 0's copy is redundant with
      // h_cpi today but must exist: the primary grid is mutated in place downstream, and the AoA
      // estimator needs every antenna to have gone through an identical chain.
      for (uint32_t a = 0; a < s.nof_ant && a < aoa_ant_; a++) {
        icf_t& cell = h_cpi_ant[((size_t)a * args.cpi_slots + r) * nof_subc + k];
        cell        = (cell * w_prev + s.h[(size_t)a * s.nof_re + i] * w) * (1.0f / w_new);
      }
    }
  }

  if (cpi_row >= args.cpi_slots) {
    // Per-row native comb (for DSP de-aliasing AND Phase 1 STO tracking below), computed from the
    // real-sample occupancy BEFORE any correction or gap-fill.
    for (uint32_t rr = 0; rr < cpi_row; rr++) {
      cpi_row_comb[rr] = row_native_comb(&occ_all[(size_t)rr * nof_subc], nof_subc);
    }

    // ota_sync_passive_ue.md Phase 6: master enable for Phases 1-4's tracking + correction. When
    // false, Phase 1-3 are skipped entirely (last_*_fit stay at their default/zero state) and Phase
    // 4's bias is not applied -- this reproduces today's pre-Phase-1 behaviour exactly, bit for bit,
    // which is the Phase 6a/6b "corrections disabled" baseline these tests/runbook steps need.
    if (args.sync_correction_enable) {
      // Phase 4 (ota_sync_passive_ue.md): apply whatever closed-loop bias the LOS baseline tracker
      // accumulated from PAST CPIs' residuals (measured in process_cpi(), after this CPI's own
      // detections are known -- one-CPI feedback latency by design). A no-op until a baseline is
      // established. Runs first purely for narrative ordering ("apply what we learned before this
      // CPI's own fresh estimation") -- it commutes with Phases 1-3 like they commute with each other.
      if (args.sync_los) {
        los_tracker.apply_bias_correction(h_cpi.data(), occ_all.data(), cpi_row, nof_subc, cpi_row_time.data(),
                                          cpi_carrier);
      }

      // Phase 1 (ota_sync_passive_ue.md): per-row LOS CIR peak tracking + fine-STO correction on the
      // RAW grid, before Stage-4b interpolation -- interpolation should operate on already-timing-
      // corrected data. See docs/NR_UE_ISAC_sync_gap_analysis.md for why this must run here.
      // Now a WALKING tracker (SYNC_NOISE_HANDOVER.md root-cause fix): the flywheel needs a recent
      // SFO estimate to project through a fade, which can only be the PREVIOUS CPI's cross-CPI EMA
      // (sfo_tracker.process() for THIS CPI hasn't run yet -- Phase 3 runs after Phase 1/2 below).
      // Phase 1's ESTIMATION always runs (Phase 2 reuses its per-row LOS taps); args.sync_sto gates
      // only whether it APPLIES its correction, so the two can be ablated independently.
      last_sto_fit = sto_tracker.process(h_cpi.data(), occ_all.data(), cpi_row, nof_subc, cpi_row_comb.data(),
                                         cpi_row_illum.data(),
                                          cpi_row_time.data(), cpi_carrier, sfo_tracker.filtered_sfo_ppm(),
                                          args.sync_sto, args.nominal_los_range_m);

      // Phase 2 (ota_sync_passive_ue.md): residual-CFO fit + per-row CPE de-rotation, reusing Phase 1's
      // per-row LOS-tap estimates (no second CIR pass). Also runs before Stage-4b interpolation, and
      // commutes with the STO correction above (uniform per-row rotation vs. STO's per-subcarrier ramp).
      if (args.sync_cfo) {
        last_cfo_fit = cfo_tracker.process(h_cpi.data(), occ_all.data(), nof_subc, sto_tracker.last_row_estimates());
      }

      // Phase 3 (ota_sync_passive_ue.md): SFO delay-drift fit + correction. Runs its own per-row
      // CIR/peak tracking walk (does not reuse Phase 1's rows -- Phase 1's fixed-window search can't
      // follow the multi-hundred-bin drift SFO can cause over a multi-second CPI). Also before
      // Stage-4b interpolation; commutes with Phase 1/2's corrections above.
      if (args.sync_sfo) {
        last_sfo_fit = sfo_tracker.process(h_cpi.data(), occ_all.data(), cpi_row, nof_subc, cpi_row_comb.data(),
                                            cpi_row_time.data(), cpi_carrier, args.nominal_los_range_m);
      }
    }

    // Stage 4b (part 1): fill the unobserved (unscheduled) CFR entries now that same-slot merges +
    // STO correction are done. Two options:
    //  - slow_time_complete: JOINT 2-D low-rank matrix completion over the whole [row x subcarrier]
    //    grid (matrix_complete.{h,cc}). Uses the physical prior that the CFR is low-rank (rank ~
    //    number of scatterers), giving each slot a consistent full aperture -- this removes the
    //    amplitude-gating that convolves each target's Doppler line with the schedule mask and creates
    //    the 2x/3x harmonic ghosts, at the source. Supersedes the per-row interpolation below.
    //  - interpolate (legacy): independent per-row linear frequency gap-fill.
    if (args.slow_time_complete && cpi_row >= 2) {
      // mc_rank==0 -> AUTO: derive this CPI's completion rank from the number of DISTINCT-RANGE
      // detections in the PREVIOUS CPI (a scatterer sits at ONE range; its Doppler harmonics and its
      // mirror all share that SAME range, so counting distinct range bins collapses each real
      // scatterer -- and its whole ghost family -- to a single count), +1 for the LOS/clutter
      // residual, clamped to [1, mc_rank_max]. CRITICAL: do NOT derive rank from the raw detection or
      // confirmed-track COUNT -- those are inflated by exactly the harmonic/false tracks this module
      // removes, which creates a feedback loop (more ghosts -> higher rank -> completion has more DOF
      // to REPRODUCE the ghosts -> more ghosts). Distinct ranges break that loop because ghosts don't
      // add new ranges. `detections` still holds the PREVIOUS CPI's list here (process_cpi() hasn't
      // overwritten it yet) -- see defs_nr_UE_ISAC.h's mc_rank comment.
      uint32_t nscat = 0;
      if (args.mc_rank == 0) {
        std::vector<uint32_t> rbins;
        rbins.reserve(detections.size());
        for (const sensing_detection_t& d : detections) {
          rbins.push_back(d.range_bin);
        }
        std::sort(rbins.begin(), rbins.end());
        for (size_t i = 0; i < rbins.size(); i++) {
          // Merge detections within a few bins of the previous distinct range into one scatterer.
          if (i == 0 || (rbins[i] - rbins[i - 1]) > 3u) {
            nscat++;
          }
        }
      }
      const uint32_t auto_rank = std::min(args.mc_rank_max, std::max(1u, nscat + 1));
      const uint32_t rank      = (args.mc_rank > 0) ? args.mc_rank : auto_rank;
      complete_lowrank(h_cpi.data(), occ_all.data(), cpi_row, nof_subc, rank, args.mc_iters,
                       args.mc_power_iters, &mc_scratch);
      // The completed grid is fully populated across the band, so every row now supports full range
      // (comb 1); reset the native combs so the per-row de-aliasing taper doesn't clip a row that was
      // sparse before completion.
      for (uint32_t rr = 0; rr < cpi_row; rr++) {
        cpi_row_comb[rr] = 1;
      }
    } else if (args.detector == "matched_filter" || (args.clean_deconv && args.clean_occ_aware)) {
      // Matched filter AND occupancy-aware CLEAN both consume the RAW occupied grid (occ_all mask)
      // directly -- no frequency gap-fill (interpolation is exactly the pedestal source MF avoids, and
      // it would smear the per-row occupancy modulation occ-aware CLEAN needs to model). Rows as-is.
    } else {
      for (uint32_t rr = 0; rr < cpi_row; rr++) {
        if (args.interpolate) {
          interp_freq_row(&h_cpi[(size_t)rr * nof_subc], &occ_all[(size_t)rr * nof_subc], nof_subc);
        }
      }
    }

    // Mean slow-time period (slots) across the CPI; falls back to the reported per-source period.
    //
    // FRACTIONAL, deliberately (2026-07-24). This used to round to an integer number of slots, which
    // silently mis-scaled the whole Doppler/velocity axis: resample_slow_time() lays its uniform grid
    // at exactly span/(N-1) slots, so rounding that to an integer here makes range_doppler's t_slow
    // disagree with the grid it is actually given. Measured on tests/sensing_sim (100 MHz, 128 rows):
    // true mean spacing 1.654 slots rounded to 2 -> every reported vel_mps came out at 0.822x the
    // ground-truth bistatic range-rate (predicted ratio 1.654/2 = 0.827). That bias also propagated
    // into the tracker, whose measurement vector includes vel_mps. The error is up to 25% at
    // spacings near 1.5-2.5 slots -- exactly the regime DL-heavy TDD traffic produces.
    /* T_slot FROM DL ROWS ONLY (2026-09-02, deliberate and temporary). The span-derived mean
     * above is taken over EVERY row, so a UL row whose slow-time position is corrupt drags the
     * whole Doppler axis with it -- and the axis is shared, so one bad row costs every source.
     * Until the UL slow-time origin is confirmed to share the DL one, the period is measured
     * over consecutive DL rows and UL rows ride that clock instead of setting it. They still
     * contribute their CFR; they just do not define the sampling interval.
     * TRIMMED MEAN, not median: a plain mean is dragged by any outlier the forward-step bound
     * misses, but a MEDIAN OF INTEGER SLOT STEPS IS ITSELF AN INTEGER, which reinstates exactly
     * the quantisation the 2026-07-24 fractional fix removed (true mean spacing 1.654 rounded to
     * 2 scaled every reported velocity by 0.827). Dropping the top decile before averaging keeps
     * the estimate fractional AND outlier-resistant.
     * REVISIT once the UL origin is verified -- UL rows are real slow-time samples and
     * excluding them costs effective PRF. */
    {
      std::vector<double> dl_steps;
      dl_steps.reserve(cpi_row);
      double prev_dl = 0.0;
      bool   have_dl = false;
      for (uint32_t rr = 0; rr < cpi_row; rr++) {
        if (rr < cpi_row_illum.size() && cpi_row_illum[rr] != NR_ISAC_ILLUM_DL) {
          continue;
        }
        if (have_dl) {
          const double d = cpi_row_time[rr] - prev_dl;
          if (d > 0.0) {
            dl_steps.push_back(d);
          }
        }
        prev_dl = cpi_row_time[rr];
        have_dl = true;
      }
      if (!dl_steps.empty()) {
        std::sort(dl_steps.begin(), dl_steps.end());
        size_t keep = (dl_steps.size() * 9) / 10; // drop the top decile
        if (keep == 0) {
          keep = dl_steps.size();
        }
        double acc = 0.0;
        for (size_t i = 0; i < keep; i++) {
          acc += dl_steps[i];
        }
        cpi_period_slots = acc / (double)keep;
      } else {
        cpi_period_slots = (args.cpi_slots > 1 && cpi_slot_span > 0)
                               ? ((double)cpi_slot_span / (double)(args.cpi_slots - 1))
                               : ((s.period_slots > 0) ? (double)s.period_slots : 1.0);
      }
      n_dl_steps_ = (uint32_t)dl_steps.size();
    }
    if (!(cpi_period_slots > 0.0)) {
      cpi_period_slots = 1.0;
    }
    // Stage 4b (part 2): resample the non-uniform-in-time rows onto a uniform slow-time grid.
    // BYPASSED under doppler_nudft OR doppler_sparse: both consume the raw irregular rows and their
    // ACTUAL times directly (resampling would defeat the point -- the chord approximation it performs
    // is itself a harmonic source, and doppler_sparse's dictionary must match cir_rm's real row
    // times), so the raw rows map 1:1 and process_cpi() hands them cpi_row_time.
    if (args.interpolate && !args.doppler_nudft && !args.doppler_sparse && args.detector != "matched_filter" &&
        !(args.clean_deconv && args.clean_occ_aware)) {
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

  /* ---- SYNC-ONLY MODE (ISAC_SYNC_ONLY=1) ----------------------------------------------------
   * Everything from here to the end-of-CPI log is the DETECTION half: range-Doppler, clutter
   * removal, CFAR, NMS, AoA, det-quality, tracking. When the question under study is STO/CFO/SFO
   * and the LOS peak finder -- all of which run ABOVE this point on the raw CPI grid -- that half
   * is pure cost, and it is what makes the CPI close long enough to block the drain thread and
   * drop 48-78 % of submissions. Skipping it leaves `detections` empty and `rvm` zeroed; the
   * report still carries the full sync block, which is the thing being measured. */
  const bool sync_only = [] {
    const char* e = getenv("ISAC_SYNC_ONLY");
    return e != nullptr && atoi(e) != 0;
  }();
  if (!sync_only) {
  // Stage 3: range-Doppler DSP on the (Stage-4b) uniformly-resampled CPI matrix. The fused grid is
  // per-subcarrier (column spacing = 1 subcarrier), so the range axis is scaled with cpi_grid_comb (1),
  // independent of any individual source's native comb.
  // Under doppler_nudft, hand the range-Doppler processor the raw irregular rows' ACTUAL times
  // (cpi_row_time, in slots) so its non-uniform DFT evaluates the true slow-time sampling; nullptr
  // otherwise (uniform-FFT path). h_cpi_uniform already equals the raw h_cpi in that mode (resample
  // bypassed above), so the row_time indexing matches its rows 1:1.
  const bool mf         = (args.detector == "matched_filter");
  const bool occ_clean  = (args.clean_deconv && args.clean_occ_aware);
  rd->process(h_cpi_uniform.data(), args.cpi_slots, nof_subc, cpi_grid_comb, cpi_carrier,
              (float)cpi_period_slots, row_comb_uniform.data(), rvm, detections,
              (args.doppler_nudft || args.doppler_sparse || mf || occ_clean) ? cpi_row_time.data() : nullptr,
              (mf || occ_clean) ? occ_all.data() : nullptr,
              hier_tracker.get());

  // CPI-quality gate (see defs_nr_UE_ISAC.h): T_slot (= cpi_period_slots, the mean row spacing) is
  // compared to a running EMA of "typical" T_slot. A CPI whose spacing is much larger is "starved" --
  // too few well-spaced samples to resolve a real target, so its detections are dominated by
  // amplitude-gating aliases/ghosts with no real reference to reject them against. Drop the whole
  // CPI's detections and let the tracker coast. The EMA updates from EVERY CPI (gated or not) so the
  // baseline tracks the cell's own traffic pattern rather than a hand-picked absolute T_slot.
  if (args.cpi_quality_gate) {
    if (tslot_ema_ < 0.0) {
      tslot_ema_ = cpi_period_slots; // seed on first CPI
    }
    if (cpi_period_slots > (double)args.cpi_quality_max_ratio * tslot_ema_) {
      LOG_I(PHY, "SENSING: cpi-quality GATE CPI #%u T_slot=%.3f > %.2f x EMA=%.3f -> %zu detections dropped\n",
            cpi_count, cpi_period_slots, (double)args.cpi_quality_max_ratio, tslot_ema_, detections.size());
      detections.clear();
    }
    const double a = (double)args.cpi_quality_ema_alpha;
    tslot_ema_ = (1.0 - a) * tslot_ema_ + a * cpi_period_slots;
  }

  // Receive-array AoA (isac_aoa.h): give every detection a bearing. Runs AFTER detection because a
  // cell's angle of arrival is only meaningful where there IS a detection -- estimating per RE would
  // cost orders of magnitude more for nothing. Uses the RAW per-antenna grid and the TRUE row times,
  // not the corrected/resampled primary grid; see the h_cpi_ant comment in sensing_engine.h.
  // Loud one-shot diagnostic for the silent-failure case this project keeps meeting: AoA configured,
  // but the receiver is only delivering ONE antenna -- e.g. nr-uesoftmodem started without a matching
  // --ue-nb-ant-rx, or an rfsim channel model with nb_rx = 1. Every bearing would simply be absent,
  // with nothing in the logs to say why.
  if (aoa_ant_ > 1 && aoa_ant_seen_ < 2 && !aoa_warned_ && cpi_count > 1) {
    aoa_warned_ = true;
    LOG_W(PHY,
          "SENSING: AoA is configured for %u elements but every CFR submission carried 1 antenna -- no "
          "bearing will ever be reported. Check --ue-nb-ant-rx and the channel model's rx antenna count.\n",
          aoa_ant_);
  }
  if (aoa && aoa_array_ready_ && aoa_ant_seen_ >= 2 && !detections.empty() && cpi_row >= 2) {
    if (args.aoa_selfcal) {
      // The direct path's bearing is known from the surveyed geometry: it is simply the direction of
      // the illuminator from this receiver. Must run BEFORE process(), which clutter-cancels exactly
      // that path away.
      const double los_bearing = std::atan2((double)args.tx_pos_y - (double)args.rx_pos_y,
                                            (double)args.tx_pos_x - (double)args.rx_pos_x);
      aoa->calibrate_from_los(h_cpi_ant.data(), aoa_ant_, cpi_row, args.cpi_slots, nof_subc, occ_all.data(),
                              cpi_row_time.data(), cpi_period_slots, cpi_carrier, los_bearing);
    }
    // args.cpi_slots is the ROW STRIDE of each antenna plane; cpi_row is how many of them are valid.
    aoa->process(h_cpi_ant.data(), aoa_ant_, cpi_row, args.cpi_slots, nof_subc, occ_all.data(),
                 cpi_row_time.data(), cpi_period_slots, cpi_carrier, detections, aoa_out);
    uint32_t n_az = 0;
    for (size_t i = 0; i < detections.size() && i < aoa_out.size(); i++) {
      if (aoa_out[i].valid) {
        detections[i].azimuth_valid   = true;
        detections[i].azimuth_deg     = aoa_out[i].azimuth_deg;
        detections[i].azimuth_std_deg = aoa_out[i].azimuth_std_deg;
        n_az++;
      }
    }
    LOG_D(PHY, "SENSING: AoA CPI #%u -> %u/%zu detections carry a bearing (calibration updates=%u)\n", cpi_count,
          n_az, detections.size(), aoa->calibration_updates());
  }

  // Position-anchored harmonic rejection (GHOST_KINEMATIC_CONSISTENCY_HANDOVER.md Phase A). MUST run
  // here and not alongside range_doppler.cc's range-bin-anchored harmonic_reject: azimuth is only
  // attached in the block above, after process() has already returned, so a position anchor is simply
  // not available where the existing test lives. Logged unconditionally when it fires -- a gate whose
  // rejection count is invisible is indistinguishable from a gate that is silently inert.
  if (args.harmonic_pos_reject) {
    const size_t   before  = detections.size();
    const uint32_t dropped = harmonic_pos_reject(args, detections);
    harmonic_pos_dropped_ += dropped;
    if (dropped > 0) {
      LOG_I(PHY, "SENSING: harmonic-pos CPI #%u dropped %u/%zu detections (cumulative %lu)\n", cpi_count,
            dropped, before, (unsigned long)harmonic_pos_dropped_);
    }
  }

  // Adaptive per-detection quality gate (det_quality.h). Runs LAST among the detection filters, and
  // after AoA, so it sees the final detection set. Learns its own operating point online -- there is
  // deliberately no SNR threshold here, because the SNR population moves bodily with gain, traffic
  // and scene (measured: the same algorithm settles at a 12.7 dB null on one capture and 18.1 dB on
  // another). Logged whenever it drops anything, with the learned state, so a gate that has latched
  // into a degenerate corner is visible rather than silently eating every detection.
  if (args.det_quality_adapt && !detections.empty()) {
    if (!det_q_) {
      det_q_.reset(new det_quality(args.det_quality_cost_ratio));
    }
    // Inter-CPI time, mirroring what the DetectionReport declares as cpi_duration: it lets each
    // detection's own range-rate say where it was in previous CPIs (motion-compensated persistence).
    const double slots_per_sf_dq = std::max(1.0, (double)cpi_carrier.scs_hz / 15000.0);
    const double cpi_dt_s        = (double)args.cpi_slots * (1e-3 / slots_per_sf_dq);
    det_q_->score(detections, rvm.range_res_m, cpi_dt_s, det_q_p_);
    const size_t                     before = detections.size();
    std::vector<sensing_detection_t> kept;
    kept.reserve(before);
    for (size_t i = 0; i < detections.size() && i < det_q_p_.size(); i++) {
      if (det_q_p_[i] >= det_q_->boundary()) {
        kept.push_back(detections[i]);
        // Carry the posterior through to the report. Survivors are NOT equally credible -- the
        // boundary is a decision, not a description -- and the central node has no way to recover
        // this number from range/rate/SNR alone.
        kept.back().p_real = det_q_p_[i];
      }
    }
    const size_t dropped = before - kept.size();
    detections.swap(kept);
    det_q_dropped_ += dropped;
    if (dropped > 0) {
      LOG_I(PHY,
            "SENSING: det-quality CPI #%u kept %zu/%zu (cumulative dropped %lu) "
            "null[med=%.1f mad=%.2f]dB sep=%.2f var[real=%.2f null=%.2f] prior=%.2f\n",
            cpi_count, detections.size(), before, (unsigned long)det_q_dropped_,
            det_q_->null_median_db(), det_q_->null_mad_db(), det_q_->separation_sigma(),
            det_q_->var_real(), det_q_->var_null(), det_q_->prior_real());
    }
  }

  // Phase 4 (ota_sync_passive_ue.md): wraps range_doppler's existing output (no second RD/CFAR
  // path) to find this CPI's LOS detection, measure its residual from the established baseline, and
  // fold that into the closed-loop bias state applied on the NEXT CPI (one-CPI feedback latency).
  // Prefer the MEASURED LOS range over args.nominal_los_range_m (default 98 m, a stale bench
  // value): Phase 4 matches detections within +-5 range bins of its guess, so a guess 29 bins off
  // means det=no on every CPI and the baseline never establishes.
  const double los_hint_m = (sto_tracker.measured_los_range_m() >= 0.0)
                                ? sto_tracker.measured_los_range_m()
                                : args.nominal_los_range_m;
  last_los_residual = los_tracker.update_residual(detections, rvm, (double)cpi_carrier.dl_center_hz,
                                                 los_hint_m);

  // Per-CPI target track. dt comes from the CPI start timestamps (irregular by design -- a CPI
  // closes when enough reference occurrences have accumulated, which depends on DL traffic), which
  // is precisely why a Kalman gain is used rather than fixed alpha-beta gains.
  if (hier_tracker || tracker) {
    // dt must be SIMULATED elapsed time (the time base the target actually moves in), NOT wall clock.
    // cpi_start_time_utc_ns is a host-clock stamp, and under rfsimulator the host runs far slower
    // than the simulated air interface (measured ~44x on this harness), so feeding it here made the
    // filter converge to a range-rate scaled by exactly that ratio (+0.14 m/s against a true
    // +6.13 m/s) while still tracking range, because the wrong rate and wrong dt cancelled in the
    // prediction. Derive it from this CPI's own slot span instead.
    const double slots_per_sf = std::max(1.0, (double)cpi_carrier.scs_hz / 15000.0);
    const double slot_dur_s   = 1e-3 / slots_per_sf;
    // Inter-CPI time = (this CPI's anchor - previous CPI's anchor) in unwrapped slots, i.e. including
    // the gap between one CPI's last row and the next CPI's first row that cpi_slot_span misses.
    // VERIFIED (2026-07-24) against ground truth on tests/sensing_sim: inverted the known trajectory
    // at each detection's chained range to get its true simulated time, least-squares fit vs CPI
    // index -> 0.1043 s/CPI true spacing against 0.105 s/CPI from this formula, i.e. 0.7% agreement.
    // This term is correct. (An earlier note in this file claimed a residual ~20% bias; that number
    // came from comparing against UE-log line interleaving with SENSING_CHANNEL gt: lines, which is
    // not a valid time reference -- the engine thread that prints this line lags the RF thread that
    // prints the gt line by an unbounded, traffic-dependent queue depth, not a fixed offset.)
    double dt_slots = (double)cpi_slot_span;
    if (have_prev_anchor_ && cpi_anchor_abs_ > prev_cpi_anchor_abs_) {
      dt_slots = (double)(cpi_anchor_abs_ - prev_cpi_anchor_abs_);
    }
    prev_cpi_anchor_abs_ = cpi_anchor_abs_;
    have_prev_anchor_    = true;
    const double dt_s    = dt_slots * slot_dur_s;
    prev_cpi_time_ns  = cpi_start_time_utc_ns;

    if (hier_tracker) {
      const double time_s = (double)cpi_anchor_abs_ * slot_dur_s;
      const double dwell_s = (double)args.cpi_slots * slot_dur_s;
      last_tracks = hier_tracker->update(detections, time_s, (double)rvm.range_res_m, (double)rvm.vel_res_mps, dwell_s);
      for (const sensing_track_t& t : last_tracks) {
        LOG_I(PHY,
              "SENSING: hier-track CPI #%u track_id=%u pos=[%.1f, %.1f, %.1f]m vel=[%+.1f, %+.1f, %+.1f]m/s "
              "range=%.2f m rate=%+.2f m/s az=%.1f deg %s coast=%u\n",
              cpi_count, t.track_id, t.pos_x, t.pos_y, t.pos_z, t.vel_x, t.vel_y, t.vel_z,
              t.range_m, t.range_rate_mps, t.azimuth_deg,
              t.updated ? "updated" : "COASTED", t.coast_count);
      }
    } else if (tracker) {
      last_tracks       = tracker->update(detections, dt_s, &rvm);
      // Single-receiver fusion: ONE Tx-Rx pair plus a bearing is enough for a position, via the exact
      // ray-ellipse closed form already used for the per-detection anchor (isac_aoa.h aoa_localize).
      // Without a bearing a lone pair fixes only the ellipse, so pos_valid stays false and the track
      // remains a range/rate-only measurement -- which is the pre-AoA behaviour, unchanged.
      for (sensing_track_t& t : last_tracks) {
        double px = 0.0, py = 0.0;
        t.pos_valid = t.azimuth_valid &&
                      aoa_localize(args.tx_pos_x, args.tx_pos_y, args.rx_pos_x, args.rx_pos_y,
                                   t.range_m, t.azimuth_deg, px, py);
        t.pos_x = (float)px;
        t.pos_y = (float)py;
      }
      for (const sensing_track_t& t : last_tracks) {
        LOG_I(PHY,
              "SENSING: track CPI #%u track_id=%u range=%.2f m rate=%+.2f m/s sigma=%.2f m %s "
              "innov=%+.2f m nis=%.2f qmult=%.2f coast=%u\n",
              cpi_count, t.track_id, t.range_m, t.range_rate_mps, t.sigma_range_m,
              t.updated ? "updated" : "COASTED", t.innovation_m, t.nis, t.q_mult, t.coast_count);
      }
      // Visibility into the auto-derived M-of-N confirmation threshold (defs_nr_UE_ISAC.h's
      // track_confirm_m==0 path): logged whenever auto mode is active so a scene whose measured
      // false-alarm density has drifted shows up here, not just as unexplained track churn.
      if (args.track_confirm_m == 0) {
        LOG_I(PHY,
              "SENSING: mot confirm CPI #%u auto_M=%u/N=%u mean_det_per_cpi=%.1f p_hit=%.5f target_pfa=%.1e\n",
              cpi_count, tracker->last_confirm_m(), args.track_confirm_n, tracker->mean_detections_ewma(),
              tracker->last_p_hit(), (double)args.track_confirm_target_pfa);
      }
    }
  }

  // Sync-correction status: one line per CPI showing whether STO (Phase 1, sub-sample delay drift),
  // CFO (Phase 2, residual carrier frequency offset), SFO (Phase 3, sample-clock error / timing
  // offset drift), and the Phase 4 closed-loop LOS bias (delay + frequency) are actually being
  // tracked/corrected -- not just computed silently into the JSON report. Print regardless of
  // sync_correction_enable: when disabled, Phases 1-3 leave their *_fit at the default/zero state
  // (is_constant/corrected == false, n_valid/n_fit == 0), which itself is the visible "disabled" signal.
  LOG_I(PHY,
        "SENSING: sync CPI #%u STO[n=%u fly=%u frac_bin=%+.3f drift=%+.3f corrected=%s walk=%+.2f] "
        "CFO[hz=%+.2f filt=%+.2f rms_rad=%.3f] SFO[raw=%+.4f filt=%+.4f hz=%+.3f n_fit=%u/%u corrected=%s] "
        "LOS[baseline=%s det=%s range_res_m=%+.2f vel_res_mps=%+.4f bias_delay_ns=%+.1f bias_cfo_hz=%+.2f]\n",
        cpi_count, last_sto_fit.n_valid, last_sto_fit.n_flywheel, last_sto_fit.mean_frac_bin,
        last_sto_fit.drift_bins_cpi, last_sto_fit.is_constant ? "yes" : "no", last_sto_fit.total_drift_bins,
        last_cfo_fit.cfo_hz, last_cfo_fit.cfo_hz_filtered,
        last_cfo_fit.residual_phase_rms_rad, last_sfo_fit.sfo_ppm, last_sfo_fit.sfo_ppm_filtered,
        last_sfo_fit.sample_clock_error_hz, last_sfo_fit.n_fit, last_sfo_fit.n_candidate,
        last_sfo_fit.corrected ? "yes" : "no", last_los_residual.baseline_established ? "yes" : "no",
        last_los_residual.detection_found ? "yes" : "no", last_los_residual.range_residual_m,
        last_los_residual.vel_residual_mps, last_los_residual.delay_bias_s * 1e9, last_los_residual.cfo_bias_hz);

  const sensing_detection_t* top = nullptr;
  for (const sensing_detection_t& d : detections) {
    if (top == nullptr || d.snr_db > top->snr_db) {
      top = &d;
    }
  }

  if (top != nullptr) {
    LOG_I(PHY,
          "SENSING: CPI #%u fc=%.1f MHz subc=%u T_slot=%.3f occ[csi=%lu dmrs=%lu data=%lu blind=%lu pusch=%lu uldata=%lu] range[res=%.2f max=%.0f]m "
          "vel[res=%.3f max=%.1f]m/s detections=%zu top: range=%.1f m vel=%.2f m/s snr=%.1f dB\n",
          cpi_count, cpi_carrier.dl_center_hz / 1e6, nof_subc, cpi_period_slots,
          (unsigned long)src_occ[NR_ISAC_SRC_CSI_RS], (unsigned long)src_occ[NR_ISAC_SRC_PDSCH_DMRS],
          (unsigned long)src_occ[NR_ISAC_SRC_PDSCH_DATA],
          (unsigned long)src_occ[NR_ISAC_SRC_PDSCH_DMRS_BLIND], (unsigned long)src_occ[NR_ISAC_SRC_PUSCH_DMRS],
          (unsigned long)src_occ[NR_ISAC_SRC_PUSCH_DATA],
          rvm.range_res_m, rvm.range_max_m, rvm.vel_res_mps,
          rvm.vel_max_mps, detections.size(), top->range_m, top->vel_mps, top->snr_db);
  } else {
    LOG_I(PHY,
          "SENSING: CPI #%u fc=%.1f MHz subc=%u occ[csi=%lu dmrs=%lu data=%lu blind=%lu pusch=%lu uldata=%lu] range_res=%.2f m vel_res=%.3f m/s "
          "detections=0\n",
          cpi_count, cpi_carrier.dl_center_hz / 1e6, nof_subc, (unsigned long)src_occ[NR_ISAC_SRC_CSI_RS],
          (unsigned long)src_occ[NR_ISAC_SRC_PDSCH_DMRS], (unsigned long)src_occ[NR_ISAC_SRC_PDSCH_DATA],
          (unsigned long)src_occ[NR_ISAC_SRC_PDSCH_DMRS_BLIND], (unsigned long)src_occ[NR_ISAC_SRC_PUSCH_DMRS],
          (unsigned long)src_occ[NR_ISAC_SRC_PUSCH_DATA],
          rvm.range_res_m, rvm.vel_res_mps);
  }

  } else {
    LOG_I(PHY,
          "SENSING: CPI #%u SYNC-ONLY fc=%.1f MHz subc=%u rows=%u T_slot=%.3f "
          "occ[csi=%lu dmrs=%lu data=%lu blind=%lu pusch=%lu uldata=%lu] (detection half disabled)\n",
          cpi_count, cpi_carrier.dl_center_hz / 1e6, nof_subc, cpi_row, cpi_period_slots,
          (unsigned long)src_occ[NR_ISAC_SRC_CSI_RS], (unsigned long)src_occ[NR_ISAC_SRC_PDSCH_DMRS],
          (unsigned long)src_occ[NR_ISAC_SRC_PDSCH_DATA],
          (unsigned long)src_occ[NR_ISAC_SRC_PDSCH_DMRS_BLIND],
          (unsigned long)src_occ[NR_ISAC_SRC_PUSCH_DMRS],
          (unsigned long)src_occ[NR_ISAC_SRC_PUSCH_DATA]);
  } // end !sync_only (detection half)

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

  // CPI duration = (number of slow-time rows) x (mean row spacing), in slots, x slot duration.
  // A 10 ms radio frame holds slots_per_frame slots.
  //
  // It is NOT cpi_slots x slot_dur: a row is one REFERENCE-SIGNAL occurrence, not one slot, and the
  // two differ by the reference's own periodicity. That only coincides when the reference fires
  // (nearly) every slot, which is the case in tests/sensing_sim -- so the old cpi_slots*slot_dur
  // form looked right there and is 160x low under, e.g., a 160-slot CSI-RS period (see
  // tests/passive_rx, where it reported 16 ms for a real 2.56 s CPI). cpi_period_slots is the same
  // fractional mean spacing already fed to range_doppler for the velocity axis, so this keeps the
  // reported duration consistent with the axes it is reported alongside.
  const uint32_t slots_per_frame = isac_slots_per_frame(cpi_carrier);
  const int64_t  slot_dur_ns     = (slots_per_frame > 0) ? (int64_t)(10000000LL / slots_per_frame) : 0;
  const double   cpi_rows_slots  = (cpi_period_slots > 0.0) ? (cpi_period_slots * (double)args.cpi_slots)
                                                            : (double)args.cpi_slots;

  detection_report_t rep;
  rep.rx_id                 = args.rx_id;
  rep.illuminator_id        = args.illuminator_id;
  rep.pci                   = cpi_carrier.pci;
  rep.ref_type              = nr_isac_sources_to_ref_type(args.sources_mask);
  rep.tx_pos_x              = args.tx_pos_x;
  rep.tx_pos_y              = args.tx_pos_y;
  rep.tx_pos_z              = args.tx_pos_z;
  rep.rx_pos_x              = args.rx_pos_x;
  rep.rx_pos_y              = args.rx_pos_y;
  rep.rx_pos_z              = args.rx_pos_z;
  rep.cpi_start_time_utc_ns = cpi_start_time_utc_ns;
  rep.cpi_duration_ns       = (int64_t)((double)slot_dur_ns * cpi_rows_slots);
  rep.fc_hz                 = (double)cpi_carrier.dl_center_hz;
  rep.subbin_interp         = args.subbin_interp;
  rep.p_detect              = det_q_ ? det_q_->detection_rate() : -1.0;
  rep.rvm                   = &rvm;
  rep.detections            = &detections;
  rep.tracks                = &last_tracks;
  rep.src_occ               = src_occ;
  rep.src_occ_len           = (uint32_t)NR_ISAC_SRC_COUNT;
  rep.include_rvm_blob      = args.capture_enable;

  // Phase 5 (ota_sync_passive_ue.md): per-CPI STO/CFO/SFO estimates + Phase 4's LOS residual.
  rep.sto = last_sto_fit;
  rep.cfo = last_cfo_fit;
  rep.sfo = last_sfo_fit;
  rep.los = last_los_residual;

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
