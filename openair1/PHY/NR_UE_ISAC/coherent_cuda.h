/* SPDX-License-Identifier: OAI-Public-License-1.1 */
/* Task 10: CUDA acceleration of the coherent fuser's two dominant CPU stages (range_doppler, ~230 ms
 * median on the chain-test scene, and envelope, ~5-14 ms) -- see task-10-report.md for the profile
 * that drove this split. detect()'s own cost (22-283 ms) is dominated by the sequential branch-and-
 * bound / per-candidate joint waveform fit / greedy residual pursuit in coherent_core.cc's detect(),
 * which is small-N (a handful of candidates), data-dependent and not embarrassingly parallel; the
 * amended brief's own method list (item 3) puts it, and refine(), on the CPU. CudaCoherent::detect()
 * therefore runs ONLY the envelope kernel on the GPU and then calls the unmodified CPU detect() on
 * the result -- guaranteeing byte-identical (not just <1e-3) parity for that stage, and touching
 * nothing in coherent_core.cc's detect()/refine() bodies. CudaCoherent::refine() is the CPU refine()
 * called directly, kept as a method only so callers have one object for the whole per-CPI GPU-backed
 * chain (find_los/estimate_row_sync stay CPU-only, per the interface note that they are cheap and
 * O(rows*subcarriers); the other agent is fixing find_los on a separate branch, so its body is not
 * touched here at all).
 */
#pragma once

#include "coherent_core.h"
#include <memory>
#include <vector>

namespace nr_isac::coherent {

class CudaCoherent {
public:
  /** True only when this binary was built with ENABLE_CHANNEL_SIM_CUDA and a usable device exists. */
  static bool available();

  CudaCoherent();  // throws std::runtime_error if !available() (stub) or device init fails
  ~CudaCoherent();
  CudaCoherent(const CudaCoherent&) = delete;
  CudaCoherent& operator=(const CudaCoherent&) = delete;

  /** Upload the CPI once; build each row's centred range profile (Hann + per-subcarrier static
   * removal + zero-padded batched cuFFT), crop near/far windows, and NUDFT into a device-resident RD
   * cube. Always downloads the (small) RD cube plus a CPU-built `wf` into the returned RdResult (see
   * file header: detect()/refine() run on the CPU and need both host-side) -- `download_rd` only
   * controls whether that RD cube is ALSO reused by a later report/topview stage without recomputing;
   * it does not change what is computed. */
  RdResult range_doppler(const CfrWindow& w, const Axes& a, const LosEstimate& L, const RowSync& s, bool download_rd);

  /** Envelope (incl. per-channel Doppler sliding-window max) on the device, over the RD cube from the
   * last range_doppler() call; downloads E and calls the CPU detect() on it unchanged (see file
   * header). `topview_max`, if non-null, is left untouched -- coherent_pipeline.cc's periodic topview
   * is already built directly from E on the host and this GPU path does not duplicate that. */
  std::vector<Detection> detect(const Grid& g, const Geometry& geo, const DetectParams& p,
                                std::vector<float>* topview_max);

  /** The envelope E (layout [t][voxel], same as coherent_core.cc's envelope()) downloaded during the
   * last detect() call -- so a caller building a periodic topview (coherent_pipeline.cc's own
   * monitor_period_s diagnostic) does not need a second GPU round trip. Empty before the first
   * detect() call. */
  const std::vector<float>& last_envelope() const;

  /** The CPU refine() (see file header) -- kept as a method so one CudaCoherent object drives the
   * whole GPU-backed per-CPI chain. Signature includes SurveySigma (the interface CPU refine() has
   * grown since the original brief was written; the amended brief supersedes it). */
  void refine(std::vector<Detection>& dets, const Grid& g, const Geometry& geo, const Calibration& cal,
             const SurveySigma& survey);

  struct Timing {
    double upload_ms = 0, build_ms = 0, fft_ms = 0, crop_norm_ms = 0, nudft_ms = 0, noise_ms = 0,
           download_ms = 0, wf_ms = 0, rd_total_ms = 0;
    double envelope_ms = 0, envelope_download_ms = 0;
  };
  Timing last_timing() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace nr_isac::coherent
