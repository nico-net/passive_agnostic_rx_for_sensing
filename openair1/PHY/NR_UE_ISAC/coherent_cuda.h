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

  /** Upload the CPI once (coherent_cuda_front.cu). find_los/estimate_row_sync/range_doppler then work on
   * the device-resident copy; each of them uploads by itself only when `w` is not the uploaded window. */
  void upload(const CfrWindow& w);
  /** coherent_core.cc's find_los()/estimate_row_sync() with their array stages on the device (same
   * decisions: the core functions run with this object's FrontOps). */
  LosEstimate find_los(const CfrWindow& w, const Axes& a, double pfa, const std::array<double, kCh>* geo_los_s = nullptr);
  RowSync estimate_row_sync(const CfrWindow& w, const Axes& a, const LosEstimate& L);

  /** Upload the CPI once; build each row's centred range profile (Hann + per-subcarrier static
   * removal + zero-padded batched cuFFT), crop near/far windows, and NUDFT into a device-resident RD
   * cube. Always downloads the (small) RD cube plus a CPU-built `wf` into the returned RdResult (see
   * file header: detect()/refine() run on the CPU and need both host-side) -- `download_rd` only
   * controls whether that RD cube is ALSO reused by a later report/topview stage without recomputing;
   * it does not change what is computed. */
  RdResult range_doppler(const CfrWindow& w, const Axes& a, const LosEstimate& L, const RowSync& s, bool download_rd);

  /** Envelope (incl. per-channel Doppler sliding-window max) on the device, over the RD cube from the
   * last range_doppler() call; calls GpuDetect on the device-resident E directly (see file header) --
   * the envelope is downloaded host-side (last_envelope()) ONLY when `topview_max` is non-null, since
   * that is the one caller (coherent_pipeline.cc's periodic topview image) that needs a host copy;
   * `topview_max` itself is left untouched, it is purely a per-call "download this CPI too" signal. */
  std::vector<Detection> detect(const Grid& g, const Geometry& geo, const DetectParams& p,
                                std::vector<float>* topview_max);

  /** Scale the device (and cached host) RD cube by per-[ch][range] amplitude factors (whiten_range_clutter). */
  void scale_rd(const std::vector<float>& g);
  /** The envelope E (layout [t][voxel], same as coherent_core.cc's envelope()), from the last detect()
   * call that was itself given a non-null `topview_max` -- so a caller building a periodic topview
   * (coherent_pipeline.cc's own monitor_period_s diagnostic) does not need a second GPU round trip.
   * Empty before the first such call; stale (last topview-due CPI's E, not this CPI's) between them --
   * fine, since the only caller re-reads it exactly on the CPIs it asked for a download. */
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
    // GPU front since the last upload (coherent_cuda_front.cu): wall ms per stage; row_sums summed over calls
    double f_upload_ms = 0, f_kernel_ms = 0, f_noncoh_ms = 0, f_union_ms = 0, f_coh_ms = 0, f_refine_ms = 0, f_rowsums_ms = 0, f_ed_ms = 0, f_wf_ms = 0;
    int f_rowsums_calls = 0;
  };
  Timing last_timing() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace nr_isac::coherent
