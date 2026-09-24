/* SPDX-License-Identifier: OAI-Public-License-1.1 */
/* GPU front of the coherent per-CPI chain (internal to the CUDA build; CudaCoherent owns one).
 *
 * The CPI is uploaded ONCE (upload()) and stays device-resident for find_los(), estimate_row_sync() and
 * CudaCoherent::range_doppler(). find_los()/estimate_row_sync() keep all their decisions in
 * coherent_core.cc; this class is their FrontOps, producing the same O(rows x subcarriers) arrays on the
 * device (row-profile FFTs, kernels, coherent union-band profile, per-row LOS sums). It also builds the
 * RdResult::Waveform (B/B2 kernel tables and the static-removal operator Q on the device).
 */
#pragma once

#include "coherent_core.h"
#include <cuda_runtime.h>
#include <cufft.h>
#include <cstdint>
#include <vector>

namespace nr_isac::coherent {

class CudaFront final : public FrontOps {
public:
  explicit CudaFront(cudaStream_t stream);
  ~CudaFront() override;
  CudaFront(const CudaFront&) = delete;
  CudaFront& operator=(const CudaFront&) = delete;

  /** Upload w and derive its per-row metadata (spans, combs, Hann weights, mask-shape groups). */
  void upload(const CfrWindow& w);
  /** True when w is the uploaded window (data pointer, dimensions and a sample fingerprint). */
  bool bound(const CfrWindow& w) const;

  /** Per-row metadata of the uploaded CPI (same formulas as coherent_core.cc). Empty rows: lo=1, hi=0. */
  struct Meta {
    uint32_t rows = 0, sc = 0;
    std::vector<int32_t> lo, hi, grp;   // grp: mask-shape group (first-appearance order), -1 empty
    std::vector<uint32_t> comb, first;  // first: representative row per group
    std::vector<double> h1, h2;         // sum of the row's Hann weights over observed subcarriers, and of squares
    std::vector<double> fc, nk;         // mean observed baseband frequency (Hz) and observed count
  };
  const Meta& meta() const { return m_; }

  // Device views of the uploaded CPI: values [ant][row][sc] (FP64), observed [row][sc], span {lo,hi}
  // per row, per-row Hann sum (FP32) and the Hann weight table hw[row][sc] (0 where not observed).
  const cufftDoubleComplex* d_values() const;
  const uint8_t* d_observed() const;
  const int2* d_span() const;
  const float* d_wsum_f() const;
  /** ed[d][r] = win_r / wsum e^{-j2pi f_d t_r} on the device (range_doppler's NUDFT and the waveform's Q). */
  const cufftDoubleComplex* ed(const CfrWindow& w, const Axes& a);
  /** The complete RdResult::Waveform (= build_waveform(w, a)), kernels and Q computed on the device. */
  RdResult::Waveform waveform(const CfrWindow& w, const Axes& a);

  void los_kernel(const Axes& a, uint32_t ovs, const std::vector<uint32_t>& first, const std::vector<uint32_t>& count,
                  std::vector<double>& kf) override;
  void los_noncoherent(const Axes& a, uint32_t R, std::array<std::vector<double>, kCh>& pw) override;
  void los_union_kernel(const Axes& a, uint32_t R, uint32_t ovs, std::vector<cd>& Uk, std::vector<double>& kmag) override;
  void los_coherent(const Axes& a, uint32_t R, const RowSync& s, std::array<std::vector<cd>, kCh>& coh) override;
  void row_sums(const std::array<double, kCh>& delay, const std::vector<double>* drift, const std::array<bool, kCh>& found,
                std::vector<std::array<cd, kCh>>& acc, std::vector<cd>* slope) override;
  void row_info(std::vector<uint32_t>& comb, std::vector<double>& fc_mean, std::vector<double>& n_obs) override;
  void los_refine(const Axes& a, const std::array<long, kCh>& best, const std::array<long, kCh>& peak,
                  const std::array<bool, kCh>& found, std::array<double, kCh>& x, std::array<cd, kCh>& tap) override;

  /** Wall time (ms) of each stage since the last upload(); row_sums accumulates over its calls. */
  struct Timing { double upload = 0, kernel = 0, noncoh = 0, union_k = 0, coh = 0, refine = 0, row_sums = 0, ed = 0, waveform = 0; int row_sums_calls = 0; };
  const Timing& timing() const { return tm_; }

  struct Buf {
    void* p = nullptr; size_t cap = 0;
    ~Buf();
    void ensure(size_t bytes);
    template <class T> T* as() const { return (T*)p; }
  };

private:
  struct Plan { cufftHandle h = 0; long n = -1, batch = -1; };
  cufftHandle plan(Plan& p, long n, long batch);   // grow-only in batch, rebuilt when n changes
  void sync();

  cudaStream_t st_;
  Meta m_;
  Timing tm_;
  const void* bound_ptr_ = nullptr;
  uint32_t bound_rows_ = 0, bound_sc_ = 0;
  std::complex<float> bound_fp_[3]{};
  double scs_hz_ = 0;
  Plan p_rows_, p_kern_, p_fine_, p_coh_;
  Buf values_f_, values_, observed_, span_, comb_, hw_, h1_, h2_, wsum_f_,
      spec_, spec_k_, spec_f_, spec_c_, wgt_, bidx_, out_d_, uk_, uc_, ukf_, ucf_, ref_in_, ref_out_, tau_, phase_, acc_, slope_,
      wrow_, rowt_, ed_, wf_rep_, wf_B_, wf_B2_, wf_Q_, e_over_h_;
};

} // namespace nr_isac::coherent
