/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#define _LARGEFILE_SOURCE
#define _FILE_OFFSET_BITS 64
#include <cinttypes>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <uhd/version.hpp>
#if UHD_VERSION < 3110000
  #include <uhd/utils/thread_priority.hpp>
#else
  #include <uhd/utils/thread.hpp>
#endif
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/version.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/thread.hpp>
#include <boost/format.hpp>
#include <iostream>
#include <complex>
#include <fstream>
#include <cmath>
#include <time.h>
#include "common/utils/LOG/log.h"
#include "common_lib.h"
#include "assertions.h"
#include "system.h"

#include "common/utils/LOG/vcd_signal_dumper.h"

#include <sys/resource.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <functional>
#include <vector>
#include <algorithm>

#include "openair1/PHY/sse_intrin.h"

/** @addtogroup _USRP_PHY_RF_INTERFACE_
 * @{
 */
extern int usrp_tx_thread;

/* ---------------------------------------------------------------------------------------------
 * Software RX decimation, for radio images whose FPGA fabric has no DDC/decimation block and can
 * only stream at one fixed native ADC rate.
 *
 * MEASURED cause (2026-08-02): the X410's "CG" (100 GbE dual-QSFP28) FPGA image variant carries
 * only two bare Radio blocks in its RFNoC graph (confirmed via `rfnoc_graph::find_blocks("DDC")`
 * returning empty) -- unlike the default "X4" image, which has DDC blocks and can decimate down
 * from its master clock to whatever rate `openair0_cfg[0].sample_rate` asks for. CG's native rate
 * is fixed at 491.52 MSps; `set_rx_rate(122.88e6, ...)` is silently coerced back to 491.52e6 by
 * UHD, so a 100 MHz/273 PRB NR capture -- built entirely around 122.88 MSps -- cannot run on that
 * image at all without decimating somewhere. This is not X410/CG-specific in general: any image
 * whose native rate is a fixed multiple of the requested rate hits the same wall, so the fix is
 * generic (auto-detected from the requested-vs-granted rate ratio), not hardcoded to one radio.
 *
 * Two cascaded halfband decimate-by-2 stages give exactly 4:1 without a custom FPGA image. Two
 * halfbands, not one CIC-style single conversion, because a 273 PRB NR carrier occupies close to
 * the FULL post-decimation Nyquist (49.14 MHz occupied vs 61.44 MHz Nyquist after 4:1) -- a bare
 * boxcar/CIC decimator's droop and imperfect stopband rejection right at the band edge would
 * measurably corrupt the outer PRBs' SNR, which is exactly where CSI-RS/PDSCH edge subcarriers
 * live. Designed and numerically verified with scipy.signal.remez against this exact rate plan
 * (491.52 -> 245.76 -> 122.88 MSps, signal edge 49.14 MHz): stage 1 (15 taps) gets ~90 dB stopband
 * rejection with a huge transition margin (its Nyquist boundary sits at 122.88 MHz, far past the
 * 49.14 MHz signal edge); stage 2 (63 taps) needs the real design margin (transition band centered
 * on the actual 61.44 MHz post-decimation Nyquist) and gets ~105 dB stopband rejection with
 * passband ripple under +/-0.0001 dB -- both well beyond the ADC's own ~12-bit quantization noise
 * floor, so this filtering is not the SNR-limiting step anywhere in the chain.
 *
 * Gated entirely on the runtime-measured rate ratio: when the granted rate equals the requested
 * rate (every other radio: B210, X4_200, N3xx, ...), decim_ratio stays 1 and every code path below
 * is skipped -- zero behavioural change, zero overhead, for anything that isn't this specific
 * fixed-rate-image case.
 * ------------------------------------------------------------------------------------------- */
#define USRP_DECIM_STAGE1_TAPS 15
#define USRP_DECIM_STAGE2_TAPS 63
#define USRP_DECIM_MAX_CHANNELS 4
#define USRP_DECIM_SPINPOOL_MAX_WORKERS 16

static const double usrp_decim_stage1_coefs[USRP_DECIM_STAGE1_TAPS] = {
  -0.002467254140064427,  0.0000109969032381363,  0.016946422031120453, -0.0000399029721231070,
  -0.067653720057452680,  0.0000774717380695567,  0.303158827377735100,  0.499905344309461040,
   0.303158827377735100,  0.0000774717380695567, -0.067653720057452680, -0.0000399029721231070,
   0.016946422031120453,  0.0000109969032381363, -0.002467254140064427
};

static const double usrp_decim_stage2_coefs[USRP_DECIM_STAGE2_TAPS] = {
  -0.0000195778516340630,  0.0000001483067034824,  0.0000717977538721336, -0.0000003501664479559,
  -0.0001941869750372001,  0.0000008832540940562,  0.0004403332074740212, -0.0000016655935971132,
  -0.0008872411544588247,  0.0000029851308579077,  0.0016397637319077306, -0.0000047481604754422,
  -0.0028342385897048724,  0.0000071921928357313,  0.0046458266993861160, -0.0000101238408573647,
  -0.0073013093558810140,  0.0000136492720357229,  0.0111110078060990550, -0.0000174408238450310,
  -0.0165425534339318570,  0.0000214437338654342,  0.0244090225801943150, -0.0000252133815109156,
  -0.0363940058120946350,  0.0000286344986108370,  0.0568586119965269200, -0.0000312432557904523,
  -0.1018874543441340200,  0.0000329837223696620,  0.3168816747068428600,  0.4999664773337295600,
   0.3168816747068428600,  0.0000329837223696620, -0.1018874543441340200, -0.0000312432557904523,
   0.0568586119965269200,  0.0000286344986108370, -0.0363940058120946350, -0.0000252133815109156,
   0.0244090225801943150,  0.0000214437338654342, -0.0165425534339318570, -0.0000174408238450310,
   0.0111110078060990550,  0.0000136492720357229, -0.0073013093558810140, -0.0000101238408573647,
   0.0046458266993861160,  0.0000071921928357313, -0.0028342385897048724, -0.0000047481604754422,
   0.0016397637319077306,  0.0000029851308579077, -0.0008872411544588247, -0.0000016655935971132,
   0.0004403332074740212,  0.0000008832540940562, -0.0001941869750372001, -0.0000003501664479559,
   0.0000717977538721336,  0.0000001483067034824, -0.0000195778516340630
};

/* Shift-register history per stage per channel. Shift-based (not circular-indexed) on purpose for
 * this first implementation: simplicity and ease of correctness-review over the modulo-indexed
 * alternative's marginally lower shift cost, given decimation only runs at up to a few hundred
 * MSps per channel on a host CPU with cores to spare -- revisit only if profiling shows this is
 * actually a bottleneck.
 *
 * SUPERSEDED (2026-08-02): the shift-per-sample version above was measured live and found NOT
 * fine -- 140 UHD RX overflows / 90 s at 491.52 MSps (vs 0 on the same test before decimation was
 * added), CPI count collapsing from 129-300 to 4. Root cause: shifting the ENTIRE tap history on
 * every single sample is O(taps) per sample regardless of whether that sample produces an output,
 * effectively doubling the real cost of the FIR (the dot product itself is only computed on every
 * OTHER sample). Replaced with the standard double-length circular buffer technique below, which
 * turns the per-sample update into O(1) (two writes, no shifting) while keeping the dot product a
 * single contiguous, non-modulo read -- correct AND SIMD-friendly if ever revisited. */
typedef struct {
  double hist1_re[2 * USRP_DECIM_STAGE1_TAPS];
  double hist1_im[2 * USRP_DECIM_STAGE1_TAPS];
  int hist1_pos;
  double hist2_re[2 * USRP_DECIM_STAGE2_TAPS];
  double hist2_im[2 * USRP_DECIM_STAGE2_TAPS];
  int hist2_pos;
} usrp_decim_chan_state_t;

/* One halfband decimate-by-2 stage. Consumes n_in input samples, produces n_in/2 output samples
 * (n_in must be even -- guaranteed by construction: the caller always requests a multiple of 4
 * raw samples, so stage 1's input is always even and stage 2's input, being stage 1's output, is
 * too).
 *
 * Double-length circular buffer, O(1) per-sample update: hist_{re,im} must be `2*taps` long.
 * Writing each new sample at BOTH buf[pos] and buf[pos+taps] (pos cycling over [0,taps)) means the
 * chronologically-ordered (oldest..newest) window of the last `taps` samples is ALWAYS the plain
 * contiguous read buf[pos+1 .. pos+taps] -- no modulo indexing inside the hot dot-product loop.
 * Verified by hand-tracing several write/wraparound cycles before trusting it (a rotation error
 * here would silently misalign the filter, not crash -- worth re-deriving by trace, not just
 * inspection, if this is ever touched again). *hist_pos persists across calls, matching the old
 * version's cross-call continuity requirement.
 *
 * Templated on the INPUT sample type: stage 1 consumes int16_t straight from UHD's recv() buffer,
 * stage 2 consumes stage 1's own double-precision output -- same filter structure, different input
 * representation, so a template avoids a near-duplicate function rather than genuinely different
 * behaviour. */

/* AVX2+FMA dot product over `taps` doubles, contiguous on both operands -- exactly what the
 * circular buffer's window read provides, so no gather/scatter is needed. MEASURED (2026-08-02):
 * the scalar version above cost 5.0 ms per trx_usrp_read() call against a 500 us real-time budget
 * (10x too slow -- confirmed by direct wall-clock instrumentation, not estimated), which is what
 * actually caused the RX overflows; no amount of socket-buffer tuning could have fixed a 10x
 * compute deficit. Reduction ordering (parallel lanes then horizontal sum) differs from the
 * scalar version's strictly-sequential sum, which for floating point is not strictly
 * bit-identical -- utterly negligible here: this filters 12-bit ADC data through a design with
 * >=89 dB stopband rejection, so a last-bit reordering difference is far below the noise floor. */
#if defined(__AVX512F__) && defined(__AVX512BW__)
/* AVX-512, 8 doubles/instruction -- double the AVX2 path's lane width. Confirmed genuine hardware
 * (not just a compile-time assumption) on the deployment target (sens3: Xeon W-2225, avx512f/bw/
 * cd/dq/vl all present in /proc/cpuinfo) before writing this: this session's OWN sandbox has no
 * AVX-512, so correctness was verified by compiling+running a standalone comparison against the
 * plain scalar sum ON sens3 itself (200000 random trials x 2 filter lengths x 16 alignments,
 * max diff ~3e-11 -- pure floating-point reduction-order noise, nowhere near a real bug) BEFORE
 * this code ever touched the live receiver. _mm512_reduce_add_pd is a genuine hardware horizontal
 * reduction instruction, simpler than AVX2's manual lane-extract-and-add. No codebase precedent
 * existed for raw (non-simde) AVX-512 intrinsics in openair1/ before this -- guarded by the same
 * __AVX512F__/__AVX512BW__ macros sse_intrin.h already uses elsewhere in this file's include
 * chain, so a build without AVX-512 falls through to the AVX2 path below unchanged. */
static inline double usrp_simd_dot_pd(const double *coefs, const double *data, int taps)
{
  __m512d acc = _mm512_setzero_pd();
  int k = 0;
  for (; k + 8 <= taps; k += 8) {
    __m512d c = _mm512_loadu_pd(&coefs[k]);
    __m512d d = _mm512_loadu_pd(&data[k]);
    acc = _mm512_fmadd_pd(c, d, acc);
  }
  double sum = _mm512_reduce_add_pd(acc);
  for (; k < taps; k++) // remainder for taps not a multiple of 8 (15 -> 7 left over, 63 -> 7 left over)
    sum += coefs[k] * data[k];
  return sum;
}
#else
static inline double usrp_simd_dot_pd(const double *coefs, const double *data, int taps)
{
  simde__m256d acc = simde_mm256_setzero_pd();
  int k = 0;
  for (; k + 4 <= taps; k += 4) {
    simde__m256d c = simde_mm256_loadu_pd(&coefs[k]);
    simde__m256d d = simde_mm256_loadu_pd(&data[k]);
    acc = simde_mm256_fmadd_pd(c, d, acc);
  }
  double lanes[4];
  simde_mm256_storeu_pd(lanes, acc);
  double sum = lanes[0] + lanes[1] + lanes[2] + lanes[3];
  for (; k < taps; k++) // remainder for taps not a multiple of 4 (15 -> 3 left over, 63 -> 3 left over)
    sum += coefs[k] * data[k];
  return sum;
}
#endif

template <typename T>
static void usrp_halfband_decimate2x(const double *coefs, int taps, double *hist_re, double *hist_im,
                                     int *hist_pos, const T *in_re, const T *in_im, int in_stride, int n_in,
                                     double *out_re, double *out_im, int out_stride = 1)
{
  int n_out = 0;
  int pos = *hist_pos;
  for (int i = 0; i < n_in; i++) {
    const double re = (double)in_re[(size_t)i * in_stride];
    const double im = (double)in_im[(size_t)i * in_stride];
    hist_re[pos] = re;
    hist_re[pos + taps] = re;
    hist_im[pos] = im;
    hist_im[pos + taps] = im;
    if ((i & 1) == 1) { // one output per pair of input samples
      const double *hr = &hist_re[pos + 1];
      const double *hi = &hist_im[pos + 1];
      out_re[(size_t)n_out * out_stride] = usrp_simd_dot_pd(coefs, hr, taps);
      out_im[(size_t)n_out * out_stride] = usrp_simd_dot_pd(coefs, hi, taps);
      n_out++;
    }
    pos++;
    if (pos == taps)
      pos = 0;
  }
  *hist_pos = pos;
}

/* ---------------------------------------------------------------------------------------------
 * Parallel block decomposition, added 2026-08-02 after live measurement showed the single-threaded
 * decimator (even with the circular buffer + SIMD dot product above) cost ~2.8 ms per
 * trx_usrp_read() call against a 500 us real-time budget at 491.52 MSps -- still >5x too slow, a
 * gap no further single-core tuning was closing reliably.
 *
 * The technique: split each call's n_in raw samples into P chunks. Chunk 0 continues the
 * PERSISTED cross-call circular-buffer state exactly as usrp_halfband_decimate2x() does today.
 * Chunks 1..P-1 are STATELESS: each reads its own `taps-1` samples of true lookback directly from
 * the raw input array (already contiguous and available, no synchronization needed) and computes
 * independently. This is an EXACT decomposition, not an approximation -- a causal LTI FIR filter's
 * output at any sample depends only on that sample and its `taps-1` predecessors, which are fully
 * determined by the input stream itself regardless of how the computation is partitioned.
 *
 * VERIFIED OFFLINE before ever touching the live receiver with it (see
 * /tmp/.../decim_test/decim_pool_test.cpp, not shipped -- scratch verification artifact): bit-exact
 * (max abs diff 0.0) against the sequential reference across hundreds of consecutive calls (so
 * cross-call state continuity is exercised, not just one isolated block) and multiple chunk counts.
 * Two real bugs were caught and fixed by that offline test before deployment:
 *   1. Chunks 1..P-1 compute correct OUTPUTS but never touch the persisted circular-buffer state,
 *      so naively only updating it from chunk 0 silently corrupts every later call's continuity.
 *      Fixed by re-seeding the persisted state from the raw array's own last `taps` samples after
 *      the parallel section -- and that reseed must be O(taps), not O(n_in-chunk): an earlier draft
 *      walked the entire remainder sequentially to "catch up" the state, which is unnecessary (the
 *      circular buffer only ever remembers its last `taps` writes) and was itself an Amdahl's-law
 *      bottleneck capping the whole exercise's speedup regardless of parallel worker count.
 *   2. Chunk 0 must be submitted to the SAME pool batch as the stateless chunks (not run
 *      sequentially before them) or 1/P of the work is serial before the parallel section even
 *      starts, which is the OTHER Amdahl's-law bottleneck that was capping measured speedup well
 *      below Px even after fix #1. Safe to co-schedule: chunk 0's hist_re/hist_im/hist_pos are
 *      touched by nothing else in the batch.
 * ------------------------------------------------------------------------------------------- */

/* Minimal persistent worker pool: threads created ONCE (not per trx_usrp_read() call -- per-call
 * std::thread spawn overhead was measured to dominate and mask the real parallel speedup during
 * offline testing), reused via a simple job-queue + batch-barrier. Deliberately simple: this call
 * site only ever needs "submit exactly P jobs, block until all P finish, repeat next call". */
/* Spin-wait worker pool, replacing an earlier mutex+condvar version (2026-08-02). MEASURED live on
 * sens3 (perf stat during a real decimating capture): IPC 1.73 (not memory-stalled) but aggregate
 * cycles implied only ~3 of 8 cores busy on average while 6 workers were supposedly running --
 * i.e. workers were mostly BLOCKED on synchronization, not computing. Futex/condvar round-trip
 * latency is genuinely large relative to a job dispatched roughly every ~500 us, which is exactly
 * this workload's regime. Switching to a busy-poll handoff (spend CPU cycles spinning instead of
 * a syscall-backed wait) is the right trade here: verified offline (see
 * /tmp/.../decim_test/spinpool_test.cpp, not shipped) at P=6, stage 1 speedup went 9.58x (vs a
 * mutex/condvar version's own measured ~1.2-1.6x on the SAME algorithm), correctness bit-exact
 * (max diff 0.0) across hundreds of calls. P=8 measured WORSE than P=6 (oversubscribes the 8
 * physical cores once spin-waiting threads are counted against the softmodem's own real-time
 * threads) -- see the P selection logic at the call site for why 6 is the current default.
 *
 * One real bug was caught by that offline test before this ever ran live: the destructor woke
 * workers by bumping the SAME counter used to signal "new job" (there is no separate exit signal),
 * so shutdown could make a worker execute a stale/default-constructed std::function --
 * std::bad_function_call, manifesting as a crash under -O2 and a hang under -O0/gdb depending on
 * timing. Fixed by re-checking stop_ immediately after waking, before touching the job slot. */
class UsrpDecimWorkerPool
{
public:
  explicit UsrpDecimWorkerPool(int n) : n_(n), stop_(false)
  {
    AssertFatal(n <= USRP_DECIM_SPINPOOL_MAX_WORKERS, "USRP decim: requested %d workers, max is %d\n", n,
                USRP_DECIM_SPINPOOL_MAX_WORKERS);
    for (int i = 0; i < n; i++) {
      job_ready_[i].store(0);
      job_done_[i].store(0);
    }
    jobs_.resize(n);
    for (int i = 0; i < n; i++)
      workers_.emplace_back([this, i] { worker_loop(i); });
  }
  ~UsrpDecimWorkerPool()
  {
    stop_.store(true, std::memory_order_release);
    for (int i = 0; i < n_; i++)
      job_ready_[i].fetch_add(1, std::memory_order_release); // wake any worker parked in the spin-wait
    for (auto &t : workers_)
      t.join();
  }
  // jobs.size() must equal n (the worker count passed to the constructor). Blocks (spins) until
  // every worker has finished this batch.
  void run_batch(std::vector<std::function<void()>> jobs)
  {
    for (int i = 0; i < n_; i++) {
      jobs_[i] = std::move(jobs[i]);
      job_ready_[i].fetch_add(1, std::memory_order_release);
    }
    for (int i = 0; i < n_; i++) {
      const int target = job_ready_[i].load(std::memory_order_relaxed);
      while (job_done_[i].load(std::memory_order_acquire) != target) {
        // busy-wait: see the class comment for why this beats a condvar for this workload
      }
    }
  }

private:
  void worker_loop(int idx)
  {
    int seen = 0;
    for (;;) {
      int cur;
      while ((cur = job_ready_[idx].load(std::memory_order_acquire)) == seen) {
        if (stop_.load(std::memory_order_acquire))
          return;
      }
      if (stop_.load(std::memory_order_acquire)) // see class comment: a wake is not always a real job
        return;
      seen = cur;
      jobs_[idx]();
      job_done_[idx].store(seen, std::memory_order_release);
    }
  }
  int n_;
  std::vector<std::thread> workers_;
  std::vector<std::function<void()>> jobs_;
  std::atomic<int> job_ready_[USRP_DECIM_SPINPOOL_MAX_WORKERS], job_done_[USRP_DECIM_SPINPOOL_MAX_WORKERS];
  std::atomic<bool> stop_;
};

/* Stateless chunk: in_re/in_im must point at (chunk_start - (taps-1)) so indices [0,taps-1) are
 * true lookback context and index (taps-1+i) is "new sample i". The taps-length window ending at
 * new-sample i is therefore in_re[i .. i+taps-1] -- read directly, no circular buffer needed since
 * the whole chunk (lookback + new) is already contiguous and fully available up front. */
template <typename T>
static void usrp_decim_chunk_stateless(const double *coefs, int taps, const T *in_re, const T *in_im,
                                       int in_stride, int n_new, double *out_re, double *out_im)
{
  int n_out = 0;
  for (int i = 0; i < n_new; i++) {
    if ((i & 1) == 1) {
      double acc_re = 0.0, acc_im = 0.0;
      for (int k = 0; k < taps; k++) {
        acc_re += coefs[k] * (double)in_re[(size_t)(i + k) * in_stride];
        acc_im += coefs[k] * (double)in_im[(size_t)(i + k) * in_stride];
      }
      out_re[n_out] = acc_re;
      out_im[n_out] = acc_im;
      n_out++;
    }
  }
}

/* Re-seed the persisted circular buffer FRESH (pos=0) from just the last `taps` raw samples of the
 * whole n_in block. O(taps), not O(n_in) -- see the block comment above for why this matters. */
template <typename T>
static void usrp_decim_reseed_state(double *hist_re, double *hist_im, int *hist_pos, int taps,
                                    const T *in_re, const T *in_im, int in_stride, int n_in)
{
  int pos = 0;
  for (int i = 0; i < taps; i++) {
    const size_t idx = (size_t)(n_in - taps + i) * in_stride;
    const double re = (double)in_re[idx];
    const double im = (double)in_im[idx];
    hist_re[pos] = re;
    hist_re[pos + taps] = re;
    hist_im[pos] = im;
    hist_im[pos + taps] = im;
    pos++;
    if (pos == taps)
      pos = 0;
  }
  *hist_pos = pos;
}

/* Parallel driver: splits n_in into P chunks, all P (including chunk 0) run concurrently via the
 * pool, then the persisted state is re-seeded for next-call continuity. Falls back to the plain
 * sequential path (unchanged behaviour) if n_in is too small to usefully/safely split. */
template <typename T>
static void usrp_decim_parallel(UsrpDecimWorkerPool *pool, int P, const double *coefs, int taps,
                                double *hist_re, double *hist_im, int *hist_pos,
                                const T *in_re, const T *in_im, int in_stride, int n_in,
                                double *out_re, double *out_im)
{
  int chunk = n_in / P;
  chunk -= (chunk % 2);
  if (pool == NULL || P < 2 || chunk < taps * 2) {
    usrp_halfband_decimate2x<T>(coefs, taps, hist_re, hist_im, hist_pos, in_re, in_im, in_stride, n_in, out_re, out_im);
    return;
  }
  const int last_chunk = n_in - chunk * (P - 1);

  std::vector<std::function<void()>> jobs;
  jobs.reserve(P);
  jobs.push_back([=]() {
    usrp_halfband_decimate2x<T>(coefs, taps, hist_re, hist_im, hist_pos, in_re, in_im, in_stride, chunk, out_re, out_im);
  });
  int in_off = chunk, out_off = chunk / 2;
  for (int p = 1; p < P; p++) {
    const int n_new = (p == P - 1) ? last_chunk : chunk;
    const T *cre = in_re + (size_t)(in_off - (taps - 1)) * in_stride;
    const T *cim = in_im + (size_t)(in_off - (taps - 1)) * in_stride;
    double *ore = out_re + out_off;
    double *oim = out_im + out_off;
    jobs.push_back([=]() { usrp_decim_chunk_stateless<T>(coefs, taps, cre, cim, in_stride, n_new, ore, oim); });
    in_off += n_new;
    out_off += n_new / 2;
  }
  pool->run_batch(std::move(jobs));

  usrp_decim_reseed_state<T>(hist_re, hist_im, hist_pos, taps, in_re, in_im, in_stride, n_in);
}

/* ---------------------------------------------------------------------------------------------
 * Cross-channel SIMD interleaving, added 2026-08-03 after live measurement showed the
 * per-channel-worker-split batching above (usrp_decim_append_jobs / usrp_decim_parallel_multi_
 * channel) was CORRECT but gave essentially no speedup: splitting a FIXED n_workers budget across
 * cc channels (per_ch[c] workers each) and running them concurrently costs the same total
 * wall-clock time as running each channel sequentially with the FULL n_workers, because either
 * way the constraint is (total samples across all channels) / (total cores) -- there was no idle
 * parallelism being left on the table to reclaim. Confirmed live: cc=2 decim_us stayed ~4000-4400us
 * (barely down from ~4500us pre-fix), still ~8x over the 500us budget.
 *
 * The only way to actually go faster is to reduce the SCALAR work per tap, and that's what this
 * does: instead of one thread computing one channel's dot product at a time, lay the cc channels'
 * samples out INTERLEAVED in memory (channel-minor) so a single AVX FMA processes all cc channels'
 * contribution to the SAME tap in one instruction. This is genuine additional throughput on top of
 * the existing P-way thread split (multiplicative, not competing for the same cores), unlike the
 * per-channel worker split above.
 *
 * Requires transposing the per-channel raw buffers into a channel-interleaved layout once per
 * stage. Stage 1's raw input is genuinely separate per-channel buffers (UHD's recv() fills them
 * that way), so it needs a real transpose. Stage 2's input is stage 1's OWN output, which this
 * code already produces pre-interleaved -- so stage 2 needs no transpose at all, it just reuses
 * stage 1's output buffer directly (see the two call sites in trx_usrp_read()). */

/* Transpose cc separate per-channel buffers into one channel-interleaved double buffer:
 * out_re_il[i*cc + c] = (double)in_re[c][i*in_stride]. O(n_in*cc) plain copies -- cheap relative
 * to the O(n_in*cc*taps) FIR work it feeds, so doing the whole range (not just the stateless
 * jobs' portion) rather than optimizing the split is a fine trade for simpler code. */
template <typename T>
static void usrp_decim_transpose_channels(const T *const *in_re, const T *const *in_im, int in_stride,
                                          int cc, int n_in, double *out_re_il, double *out_im_il)
{
  for (int i = 0; i < n_in; i++) {
    for (int c = 0; c < cc; c++) {
      out_re_il[(size_t)i * cc + c] = (double)in_re[c][(size_t)i * in_stride];
      out_im_il[(size_t)i * cc + c] = (double)in_im[c][(size_t)i * in_stride];
    }
  }
}

/* Stateless chunk, channel-interleaved: in_re_il/in_im_il must point at (chunk_start - (taps-1))*cc
 * so index (i+k)*cc + c is channel c's sample for tap-window position (i+k), matching
 * usrp_decim_chunk_stateless's per-channel indexing exactly but with a cc-wide stride. The cc==4
 * fast path (this codebase's actual channel cap, USRP_DECIM_MAX_CHANNELS) does all 4 channels'
 * accumulation for one tap in a single 256-bit FMA; other channel counts (1-3) use the equivalent
 * plain scalar loop -- correct, just not vectorized, since 4 is what the 4-antenna goal needs. */
static void usrp_decim_chunk_stateless_interleaved(int cc, const double *coefs, int taps,
                                                   const double *in_re_il, const double *in_im_il,
                                                   int n_new, double *out_re_il, double *out_im_il)
{
  int n_out = 0;
  for (int i = 0; i < n_new; i++) {
    if ((i & 1) == 1) {
#if defined(__AVX2__) && defined(__FMA__)
      if (cc == 4) {
        __m256d acc_re = _mm256_setzero_pd();
        __m256d acc_im = _mm256_setzero_pd();
        for (int k = 0; k < taps; k++) {
          const __m256d c = _mm256_set1_pd(coefs[k]);
          acc_re = _mm256_fmadd_pd(c, _mm256_loadu_pd(&in_re_il[(size_t)(i + k) * 4]), acc_re);
          acc_im = _mm256_fmadd_pd(c, _mm256_loadu_pd(&in_im_il[(size_t)(i + k) * 4]), acc_im);
        }
        _mm256_storeu_pd(&out_re_il[(size_t)n_out * 4], acc_re);
        _mm256_storeu_pd(&out_im_il[(size_t)n_out * 4], acc_im);
        n_out++;
        continue;
      }
#endif
      double acc_re[USRP_DECIM_MAX_CHANNELS] = {0}, acc_im[USRP_DECIM_MAX_CHANNELS] = {0};
      for (int k = 0; k < taps; k++) {
        const double coef = coefs[k];
        for (int c = 0; c < cc; c++) {
          acc_re[c] += coef * in_re_il[(size_t)(i + k) * cc + c];
          acc_im[c] += coef * in_im_il[(size_t)(i + k) * cc + c];
        }
      }
      for (int c = 0; c < cc; c++) {
        out_re_il[(size_t)n_out * cc + c] = acc_re[c];
        out_im_il[(size_t)n_out * cc + c] = acc_im[c];
      }
      n_out++;
    }
  }
}

/* Chunk 0 (stateful, persisted circular buffer): unlike the stateless path above, this reads each
 * channel's OWN separate history buffer, so it stays a per-channel loop over the existing,
 * unchanged usrp_halfband_decimate2x() -- only its NEW out_stride=cc parameter is new, letting it
 * write directly into the shared interleaved output array at column c instead of needing its own
 * per-channel scratch + a second transpose. Small fraction of total work (1 chunk out of P), so
 * leaving it un-vectorized costs little. */
template <typename T>
static void usrp_decim_chan0_stateful_interleaved(const double *coefs, int taps,
                                                  double *const *hist_re, double *const *hist_im, int *const *hist_pos,
                                                  const T *const *in_re, const T *const *in_im, int in_stride,
                                                  int cc, int chunk_len,
                                                  double *out_re_il, double *out_im_il)
{
  for (int c = 0; c < cc; c++) {
    usrp_halfband_decimate2x<T>(coefs, taps, hist_re[c], hist_im[c], hist_pos[c],
                                in_re[c], in_im[c], in_stride, chunk_len,
                                out_re_il + c, out_im_il + c, cc);
  }
}

/* Top-level interleaved driver: full n_workers is used for the P-way CHUNK split (same as the
 * original single-channel usrp_decim_parallel, NOT divided by channel count -- the win here comes
 * from each stateless chunk job internally covering all cc channels via SIMD, not from giving each
 * channel fewer workers). in_re_il/in_im_il is the ALREADY-INTERLEAVED input the caller supplies:
 * for stage 1 that's a freshly transposed scratch buffer (usrp_decim_transpose_channels, raw
 * per-channel buffers are genuinely separate); for stage 2 it's simply stage 1's own output buffer
 * reused in place, since this function already produces interleaved output -- no second transpose. */
template <typename T>
static void usrp_decim_parallel_interleaved(UsrpDecimWorkerPool *pool, int n_workers,
                                            const double *coefs, int taps,
                                            double *const *hist_re, double *const *hist_im, int *const *hist_pos,
                                            const T *const *in_re, const T *const *in_im, int in_stride,
                                            int cc, int n_in,
                                            const double *in_re_il, const double *in_im_il,
                                            double *out_re_il, double *out_im_il)
{
  const int P = n_workers;
  int chunk = n_in / P;
  chunk -= (chunk % 2);
  if (pool == NULL || P < 2 || chunk < taps * 2) {
    usrp_decim_chan0_stateful_interleaved<T>(coefs, taps, hist_re, hist_im, hist_pos, in_re, in_im, in_stride,
                                             cc, n_in, out_re_il, out_im_il);
    for (int c = 0; c < cc; c++)
      usrp_decim_reseed_state<T>(hist_re[c], hist_im[c], hist_pos[c], taps, in_re[c], in_im[c], in_stride, n_in);
    return;
  }
  const int last_chunk = n_in - chunk * (P - 1);

  std::vector<std::function<void()>> jobs;
  jobs.reserve(P);
  jobs.push_back([=]() {
    usrp_decim_chan0_stateful_interleaved<T>(coefs, taps, hist_re, hist_im, hist_pos, in_re, in_im, in_stride,
                                             cc, chunk, out_re_il, out_im_il);
  });
  int in_off = chunk, out_off = (chunk / 2) * cc;
  for (int p = 1; p < P; p++) {
    const int n_new = (p == P - 1) ? last_chunk : chunk;
    const double *sre = in_re_il + (size_t)(in_off - (taps - 1)) * cc;
    const double *sim = in_im_il + (size_t)(in_off - (taps - 1)) * cc;
    double *ore = out_re_il + out_off;
    double *oim = out_im_il + out_off;
    jobs.push_back([=]() { usrp_decim_chunk_stateless_interleaved(cc, coefs, taps, sre, sim, n_new, ore, oim); });
    in_off += n_new;
    out_off += (n_new / 2) * cc;
  }
  pool->run_batch(std::move(jobs));

  for (int c = 0; c < cc; c++)
    usrp_decim_reseed_state<T>(hist_re[c], hist_im[c], hist_pos[c], taps, in_re[c], in_im[c], in_stride, n_in);
}

typedef struct {

  // --------------------------------
  // variables for USRP configuration
  // --------------------------------
  //! USRP device pointer
  uhd::usrp::multi_usrp::sptr usrp;

  //create a send streamer and a receive streamer
  //! USRP TX Stream
  uhd::tx_streamer::sptr tx_stream;
  //! USRP RX Stream
  uhd::rx_streamer::sptr rx_stream;

  //! USRP TX Metadata
  uhd::tx_metadata_t tx_md;
  //! USRP RX Metadata
  uhd::rx_metadata_t rx_md;

  //! Sampling rate (LOGICAL rate OAI's timing model is built around -- e.g. 122.88e6. May differ
  //! from the USRP's actual hardware rate; see decim_ratio below)
  double sample_rate;

  //! Software RX decimation ratio (1 = disabled, i.e. hardware rate == requested rate; currently
  //! only 1 or 4 are supported -- see usrp_halfband_decimate2x() and the comment block above it.
  //! Detected automatically after set_rx_rate() by comparing the requested and granted rates.
  int decim_ratio;
  usrp_decim_chan_state_t decim_state[USRP_DECIM_MAX_CHANNELS];
  // All three scratch buffers are HEAP-allocated and grown on demand (never a stack VLA): at
  // decim_ratio=4 the raw buffer alone is ~4x the size of the pre-existing buff_tmp stack VLA in
  // trx_usrp_read() (which itself already scales with nsamps, typically ~61440 complex samples per
  // call at 273 PRB -- i.e. ~1 MB raw here), and stacking that on top of an already-sized stack
  // frame is exactly the oversized-VLA failure class fixed in nr_initial_sync.c earlier this
  // session. Sized in complex-sample units; grown (not shrunk) the first time a call needs more.
  int16_t *decim_raw_buf[USRP_DECIM_MAX_CHANNELS];       // raw pre-decimation samples, interleaved I/Q
  int decim_raw_buf_nsamps[USRP_DECIM_MAX_CHANNELS];     // capacity of decim_raw_buf, in complex samples
  // Channel-interleaved scratch (SHARED across all channels, not per-channel arrays) for the
  // cross-channel SIMD decimation path -- see the usrp_decim_parallel_interleaved block comment.
  // decim_il_raw_{re,im}: transposed raw input for stage 1 (raw_needed*cc doubles each).
  // decim_il_stage1_{re,im}: stage 1 output AND stage 2 input, already interleaved -- no
  // re-transpose needed between stages ((raw_needed/2)*cc doubles each).
  // decim_il_stage2_{re,im}: final interleaved output, de-interleaved directly into buff_tmp
  // (nsamps*cc doubles each).
  double *decim_il_raw_re, *decim_il_raw_im;
  int decim_il_raw_nsamps;         // capacity, in TOTAL (all channels combined) complex samples
  double *decim_il_stage1_re, *decim_il_stage1_im;
  int decim_il_stage1_nsamps;
  double *decim_il_stage2_re, *decim_il_stage2_im;
  int decim_il_stage2_nsamps;
  // Lazily created on first use (only when decim_ratio > 1 -- every other radio never touches
  // this). Persistent across calls: per-call thread spawn was measured to dominate and mask the
  // real parallel speedup during offline testing, hence a reusable pool rather than std::thread
  // per trx_usrp_read() call. usrp_state_t is private to this .cpp (never referenced from a
  // shared header, confirmed), so a real typed pointer is fine here, not void*.
  UsrpDecimWorkerPool *decim_pool;
  int decim_num_workers; // P used for usrp_decim_parallel(); 0 until first decimating call

  //! TX forward samples. We use usrp_time_offset to get this value
  int tx_forward_nsamps; //166 for 20Mhz

  //! gpio bank to use
  char *gpio_bank;
  
  // --------------------------------
  // Debug and output control
  // --------------------------------
  int num_underflows;
  int num_overflows;
  int num_seq_errors;
  int64_t tx_count;
  int64_t rx_count;
  int wait_for_first_pps;
  int use_gps;
  //int first_tx;
  //int first_rx;
  //! timestamp of RX packet
  openair0_timestamp_t rx_timestamp;
} usrp_state_t;

//void print_notes(void)
//{
// Helpful notes
//  std::cout << boost::format("**************************************Helpful Notes on Clock/PPS Selection**************************************\n");
//  std::cout << boost::format("As you can see, the default 10 MHz Reference and 1 PPS signals are now from the GPSDO.\n");
//  std::cout << boost::format("If you would like to use the internal reference(TCXO) in other applications, you must configure that explicitly.\n");
//  std::cout << boost::format("You can no longer select the external SMAs for 10 MHz or 1 PPS signaling.\n");
//  std::cout << boost::format("****************************************************************************************************************\n");
//}

int check_ref_locked(usrp_state_t *s,size_t mboard) {
  std::vector<std::string> sensor_names = s->usrp->get_mboard_sensor_names(mboard);
  bool ref_locked = false;

  if(std::find(sensor_names.begin(), sensor_names.end(), "ref_locked") != sensor_names.end()) {
    std::cout << "Waiting for reference lock..." << std::flush;

    for (int i = 0; i < 30 and not ref_locked; i++) {
      ref_locked = s->usrp->get_mboard_sensor("ref_locked", mboard).to_bool();

      if (not ref_locked) {
        std::cout << "." << std::flush;
        boost::this_thread::sleep(boost::posix_time::seconds(1));
      }
    }

    if(ref_locked) {
      std::cout << "LOCKED" << std::endl;
    } else {
      std::cout << "FAILED" << std::endl;
    }
  } else {
    std::cout << boost::format("ref_locked sensor not present on this board.\n");
  }

  return ref_locked;
}

static int sync_to_gps(openair0_device_t *device)
{
  //uhd::set_thread_priority_safe();
  //std::string args;
  //Set up program options
  //po::options_description desc("Allowed options");
  //desc.add_options()
  //("help", "help message")
  //("args", po::value<std::string>(&args)->default_value(""), "USRP device arguments")
  //;
  //po::variables_map vm;
  //po::store(po::parse_command_line(argc, argv, desc), vm);
  //po::notify(vm);
  //Print the help message
  //if (vm.count("help"))
  //{
  //  std::cout << boost::format("Synchronize USRP to GPS %s") % desc << std::endl;
  // return EXIT_FAILURE;
  //}
  //Create a USRP device
  //std::cout << boost::format("\nCreating the USRP device with: %s...\n") % args;
  //uhd::usrp::multi_usrp::sptr usrp = uhd::usrp::multi_usrp::make(args);
  //std::cout << boost::format("Using Device: %s\n") % usrp->get_pp_string();
  usrp_state_t *s = (usrp_state_t *)device->priv;

  try {
    size_t num_mboards = s->usrp->get_num_mboards();
    size_t num_gps_locked = 0;

    for (size_t mboard = 0; mboard < num_mboards; mboard++) {
      std::cout << "Synchronizing mboard " << mboard << ": " << s->usrp->get_mboard_name(mboard) << std::endl;
      bool ref_locked = check_ref_locked(s,mboard);

      if (ref_locked) {
        std::cout << boost::format("Ref Locked\n");
      } else {
        std::cout << "Failed to lock to GPSDO 10 MHz Reference. Exiting." << std::endl;
        exit(EXIT_FAILURE);
      }

      //Wait for GPS lock
      bool gps_locked = s->usrp->get_mboard_sensor("gps_locked", mboard).to_bool();

      if(gps_locked) {
        num_gps_locked++;
        std::cout << boost::format("GPS Locked\n");
      } else {
        LOG_W(HW,"WARNING:  GPS not locked - time will not be accurate until locked\n");
      }

      //Set to GPS time
      uhd::time_spec_t gps_time = uhd::time_spec_t(time_t(s->usrp->get_mboard_sensor("gps_time", mboard).to_int()));
      s->usrp->set_time_next_pps(gps_time+1.0, mboard);
      //s->usrp->set_time_next_pps(uhd::time_spec_t(0.0));
      
      //Wait for it to apply
      //The wait is 2 seconds because N-Series has a known issue where
      //the time at the last PPS does not properly update at the PPS edge
      //when the time is actually set.
      boost::this_thread::sleep(boost::posix_time::seconds(2));
      //Check times
      gps_time = uhd::time_spec_t(time_t(s->usrp->get_mboard_sensor("gps_time", mboard).to_int()));
      uhd::time_spec_t time_last_pps = s->usrp->get_time_last_pps(mboard);
      std::cout << "USRP time: " << (boost::format("%0.9f") % time_last_pps.get_real_secs()) << std::endl;
      std::cout << "GPSDO time: " << (boost::format("%0.9f") % gps_time.get_real_secs()) << std::endl;
      if (gps_time.get_real_secs() == time_last_pps.get_real_secs())
          std::cout << std::endl << "SUCCESS: USRP time synchronized to GPS time" << std::endl << std::endl;
      else
          std::cerr << std::endl << "ERROR: Failed to synchronize USRP time to GPS time" << std::endl << std::endl;
    }

    if (num_gps_locked == num_mboards and num_mboards > 1) {
      //Check to see if all USRP times are aligned
      //First, wait for PPS.
      uhd::time_spec_t time_last_pps = s->usrp->get_time_last_pps();

      while (time_last_pps == s->usrp->get_time_last_pps()) {
        boost::this_thread::sleep(boost::posix_time::milliseconds(1));
      }

      //Sleep a little to make sure all devices have seen a PPS edge
      boost::this_thread::sleep(boost::posix_time::milliseconds(200));
      //Compare times across all mboards
      bool all_matched = true;
      uhd::time_spec_t mboard0_time = s->usrp->get_time_last_pps(0);

      for (size_t mboard = 1; mboard < num_mboards; mboard++) {
        uhd::time_spec_t mboard_time = s->usrp->get_time_last_pps(mboard);

        if (mboard_time != mboard0_time) {
          all_matched = false;
          std::cerr << (boost::format("ERROR: Times are not aligned: USRP 0=%0.9f, USRP %d=%0.9f")
                        % mboard0_time.get_real_secs()
                        % mboard
                        % mboard_time.get_real_secs()) << std::endl;
        }
      }

      if (all_matched) {
        std::cout << "SUCCESS: USRP times aligned" << std::endl << std::endl;
      } else {
        std::cout << "ERROR: USRP times are not aligned" << std::endl << std::endl;
      }
    }
  } catch (std::exception &e) {
    std::cout << boost::format("\nError: %s") % e.what();
    std::cout << boost::format("This could mean that you have not installed the GPSDO correctly.\n\n");
    std::cout << boost::format("Visit one of these pages if the problem persists:\n");
    std::cout << boost::format(" * N2X0/E1X0: http://files.ettus.com/manual/page_gpsdo.html");
    std::cout << boost::format(" * X3X0: http://files.ettus.com/manual/page_gpsdo_x3x0.html\n\n");
    std::cout << boost::format(" * E3X0: http://files.ettus.com/manual/page_usrp_e3x0.html#e3x0_hw_gps\n\n");
    exit(EXIT_FAILURE);
  }

  return EXIT_SUCCESS;
}

#define ATR_MASK 0x7f //pins controlled by ATR
#define ATR_RX   0x50 //data[4] and data[6]
#define ATR_XX   0x20 //data[5]
#define MAN_MASK ATR_MASK ^ 0xFFF // manually controlled pins

static void trx_usrp_start_interdigital_gpio(openair0_device_t *device, usrp_state_t *s)
{
  AssertFatal(device->type == USRP_X400_DEV,
              "interdigital frontend device for beam management can only be used together with an X400\n");
  // set data direction register (DDR) to output
  s->usrp->set_gpio_attr(s->gpio_bank, "DDR", 0xfff, 0xfff);
  // set lower GPIO#1 to be controlled automatically by ATR (the rest  bits are controlled manually)
  s->usrp->set_gpio_attr(s->gpio_bank, "CTRL", (1 << 1), (1 << 1));
  // set GPIO1 (Tx/Rx1)  to 1 for MHU1 for transmistting
  s->usrp->set_gpio_attr(s->gpio_bank, "ATR_XX", (1 << 1), (1 << 1));
  // set GPIO4 (ID0) to 1 and GPIO2 (TX/RX2) &GPIO3 (ID1) to 0
  s->usrp->set_gpio_attr(s->gpio_bank, "OUT", (1 << 4), 0x1c);
}

static void trx_usrp_start_generic_gpio(usrp_state_t *s)
{
  // setup GPIO for TDD, GPIO(4) = ATR_RX
  // set data direction register (DDR) to output
  s->usrp->set_gpio_attr(s->gpio_bank, "DDR", 0xfff, 0xfff);
  // set bits to be controlled automatically by ATR
  s->usrp->set_gpio_attr(s->gpio_bank, "CTRL", ATR_MASK, 0xfff);
  // set bits to 1 when the radio is only receiving (ATR_RX)
  s->usrp->set_gpio_attr(s->gpio_bank, "ATR_RX", ATR_RX, ATR_MASK);
  // set bits to 1 when the radio is transmitting and receiveing (ATR_XX)
  // (we use full duplex here, because our RX is on all the time - this might need to change later)
  s->usrp->set_gpio_attr(s->gpio_bank, "ATR_XX", ATR_XX, ATR_MASK);
  // set all other pins to manual
  s->usrp->set_gpio_attr(s->gpio_bank, "OUT", MAN_MASK, 0xfff);
}

/*! \brief Called to start the USRP transceiver. Return 0 if OK, < 0 if error
    @param device pointer to the device structure specific to the RF hardware target
*/
static int trx_usrp_start(openair0_device_t *device)
{
  usrp_state_t *s = (usrp_state_t *)device->priv;

  s->gpio_bank = (char *) "FP0"; //good for B210, X310 and N310

#if UHD_VERSION>4000000
  if (device->type == USRP_X400_DEV) {
    // Set every pin on GPIO0 to be controlled by DB0_RF0
    std::vector<std::string> sxx{12, "DB0_RF0"};
    s->gpio_bank = (char *) "GPIO0";
    s->usrp->set_gpio_src(s->gpio_bank, sxx);
  }
#endif

  switch (device->openair0_cfg->gpio_controller) {
    case RU_GPIO_CONTROL_NONE:
      break;
    case RU_GPIO_CONTROL_GENERIC:
      trx_usrp_start_generic_gpio(s);
      break;
    case RU_GPIO_CONTROL_INTERDIGITAL:
      trx_usrp_start_interdigital_gpio(device, s);
      break;
    default:
      AssertFatal(false, "illegal GPIO controller %d\n", device->openair0_cfg->gpio_controller);
  }

  s->wait_for_first_pps = 1;
  s->rx_count = 0;
  s->tx_count = 0;
  //s->first_tx = 1;
  //s->first_rx = 1;
  s->rx_timestamp = 0;

    //wait for next pps
  uhd::time_spec_t last_pps = s->usrp->get_time_last_pps();
  uhd::time_spec_t current_pps = s->usrp->get_time_last_pps();
  while(current_pps == last_pps) {
    boost::this_thread::sleep(boost::posix_time::milliseconds(1));
    current_pps = s->usrp->get_time_last_pps();
  }

  LOG_I(HW,"current pps at %f, starting streaming at %f\n",current_pps.get_real_secs(),current_pps.get_real_secs()+1.0);

  uhd::stream_cmd_t cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
  cmd.time_spec = uhd::time_spec_t(current_pps+1.0);
  cmd.stream_now = false; // start at constant delay
  s->rx_stream->issue_stream_cmd(cmd);

  return 0;
}

static void trx_usrp_send_end_of_burst(usrp_state_t *s)
{
  // if last packet sent was end of burst no need to do anything. otherwise send end of burst packet
  if (s->tx_md.end_of_burst)
    return;
  s->tx_md.end_of_burst = true;
  s->tx_md.start_of_burst = false;
  s->tx_md.has_time_spec = false;
  s->tx_stream->send("", 0, s->tx_md);
}

static void trx_usrp_finish_rx(usrp_state_t *s)
{
  /* finish rx by sending STREAM_MODE_STOP_CONTINUOUS */
  uhd::stream_cmd_t cmd(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
  s->rx_stream->issue_stream_cmd(cmd);

  /* Collect the samples still in flight, so the next start_rx begins on a clean stream.
   *
   * BOUNDED, and that is load-bearing: this loop used to be an unbounded
   * "do { recv } while (samples > 0)". On an X410 (RFNoC) the stream does not reliably run dry
   * after STOP_CONTINUOUS, so recv keeps returning samples and the loop never exits. MEASURED:
   * the UE completed initial sync against a live 100 MHz cell, then hit this during its first
   * resync and froze permanently -- UEthread_0 parked in recv(), every other thread idle, not one
   * further line of log output, and no error anywhere. It looks exactly like a protocol/decode
   * problem and is not one. Stop on the first timeout/error, and cap the iteration count so a
   * device that keeps producing can never wedge the UE. */
  size_t samples;
  uint8_t buf[1024];
  std::vector<void *> buff_ptrs;
  for (size_t i = 0; i < s->usrp->get_rx_num_channels(); i++) buff_ptrs.push_back(buf);
  const int max_drain_iterations = 10000;
  int iterations = 0;
  do {
    samples = s->rx_stream->recv(buff_ptrs, sizeof(buf) / 4, s->rx_md, 0.01);
    if (s->rx_md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) {
      break; // timeout (stream is dry) or a real error -- either way, stop draining
    }
  } while (samples > 0 && ++iterations < max_drain_iterations);

  if (iterations >= max_drain_iterations) {
    LOG_W(HW, "RX stream still delivering after %d drain iterations, continuing anyway\n", max_drain_iterations);
  }
}

static void trx_usrp_write_reset(openair0_thread_t *wt);

/*! \brief Terminate operation of the USRP transceiver -- free all associated resources
 * \param device the hardware to use
 */
static void trx_usrp_end(openair0_device_t *device)
{
  if (device == NULL)
    return;

  usrp_state_t *s = (usrp_state_t *)device->priv;

  AssertFatal(s != NULL, "%s() called on uninitialized USRP\n", __func__);
  iqrecorder_end(device);

  LOG_I(HW, "releasing USRP\n");
  if (usrp_tx_thread != 0)
    trx_usrp_write_reset(&device->write_thread);

  /* finish tx and rx */
  trx_usrp_send_end_of_burst(s);
  trx_usrp_finish_rx(s);
  /* set tx_stream, rx_stream, and usrp to NULL to clear/free them */
  s->tx_stream = NULL;
  s->rx_stream = NULL;
  s->usrp = NULL;
  for (int i = 0; i < USRP_DECIM_MAX_CHANNELS; i++) {
    free(s->decim_raw_buf[i]);
  }
  free(s->decim_il_raw_re); free(s->decim_il_raw_im);
  free(s->decim_il_stage1_re); free(s->decim_il_stage1_im);
  free(s->decim_il_stage2_re); free(s->decim_il_stage2_im);
  delete s->decim_pool; // safe on NULL (never created if decim_ratio never exceeded 1)
  free(s);
  device->priv = NULL;
  device->trx_start_func = NULL;
  device->trx_get_stats_func = NULL;
  device->trx_reset_stats_func = NULL;
  device->trx_end_func = NULL;
  device->trx_stop_func = NULL;
  device->trx_set_freq_func = NULL;
  device->trx_set_gains_func = NULL;
  device->trx_write_init = NULL;
}

/*! \brief Called to send samples to the USRP RF target
      @param device pointer to the device structure specific to the RF hardware target
      @param timestamp The timestamp at which the first sample MUST be sent
      @param buff Buffer which holds the samples
      @param nsamps number of samples to be sent
      @param antenna_id index of the antenna if the device has multiple antennas
      @param flags flags must be set to true if timestamp parameter needs to be applied
*/
static int trx_usrp_write(openair0_device_t *device,
			  openair0_timestamp_t timestamp,
			  void **buff,
			  int nsamps,
			  int cc,
			  int flags)
{
  int ret=0;
  usrp_state_t *s = (usrp_state_t *)device->priv;
  timestamp -= device->openair0_cfg->command_line_sample_advance + device->openair0_cfg->tx_sample_advance;
  int nsamps2;  // aligned to upper 32 or 16 byte boundary

  radio_tx_burst_flag_t flags_burst = (radio_tx_burst_flag_t) (flags & 0xf);
  radio_tx_gpio_flag_t flags_gpio = (radio_tx_gpio_flag_t) ((flags >> 4) & 0x1fff);

  int end;
  openair0_thread_t *write_thread = &device->write_thread;
  openair0_write_package_t *write_package = write_thread->write_package;

  AssertFatal( MAX_WRITE_THREAD_BUFFER_SIZE >= cc,"Do not support more than %d cc number\n", MAX_WRITE_THREAD_BUFFER_SIZE);

  bool first_packet_state=false,last_packet_state=false;

    if (flags_burst == TX_BURST_START) {
      //      s->tx_md.start_of_burst = true;
      //      s->tx_md.end_of_burst = false;
      first_packet_state = true;
      last_packet_state  = false;
    } else if (flags_burst == TX_BURST_END) {
      //s->tx_md.start_of_burst = false;
      //s->tx_md.end_of_burst = true;
      first_packet_state = false;
      last_packet_state  = true;
    } else if (flags_burst == TX_BURST_START_AND_END) {
    //  s->tx_md.start_of_burst = true;
    //  s->tx_md.end_of_burst = true;
      first_packet_state = true;
      last_packet_state  = true;
    } else if (flags_burst == TX_BURST_MIDDLE) {
    //  s->tx_md.start_of_burst = false;
    //  s->tx_md.end_of_burst = false;
      first_packet_state = false;
      last_packet_state  = false;
    }
    else if (flags_burst==10) { // fail safe mode
     // s->tx_md.has_time_spec = false;
     // s->tx_md.start_of_burst = false;
     // s->tx_md.end_of_burst = true;
     first_packet_state = false;
     last_packet_state  = true;
    }

    if (usrp_tx_thread == 0) {
      nsamps2 = (nsamps+7)>>3;
      simde__m256i buff_tx[cc < 2 ? 2 : cc][nsamps2];

      // bring TX data into 16 MSBs, assuming it is on the 12 LSB  after OAI computation
      const int shift = 4;
      for (int i = 0; i < cc; i++) {
        for (int j = 0; j < nsamps2; j++) {
          if ((((uintptr_t)buff[i]) & 0x1F) == 0) {
            buff_tx[i][j] = simde_mm256_slli_epi16(((simde__m256i *)buff[i])[j], shift);
          } else {
            simde__m256i tmp = simde_mm256_loadu_si256(((simde__m256i *)buff[i]) + j);
            buff_tx[i][j] = simde_mm256_slli_epi16(tmp, shift);
          }
        }
      }

      s->tx_md.has_time_spec = true;
      s->tx_md.start_of_burst = (s->tx_count == 0) ? true : first_packet_state;
      s->tx_md.end_of_burst = last_packet_state;
      s->tx_md.time_spec = uhd::time_spec_t::from_ticks(timestamp, s->sample_rate);
      s->tx_count++;

      VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME(VCD_SIGNAL_DUMPER_FUNCTIONS_BEAM_SWITCHING_GPIO, 1);
      // bit 13 enables gpio
      if ((flags_gpio & TX_GPIO_CHANGE) != 0) {
        // push GPIO bits
        s->usrp->set_command_time(s->tx_md.time_spec);
        s->usrp->set_gpio_attr(s->gpio_bank, "OUT", flags_gpio, MAN_MASK);
        s->usrp->clear_command_time();
      }
      VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME(VCD_SIGNAL_DUMPER_FUNCTIONS_BEAM_SWITCHING_GPIO, 0);

      if (cc > 1) {
        std::vector<void *> buff_ptrs;

        for (int i = 0; i < cc; i++)
          buff_ptrs.push_back(&(((int16_t *)buff_tx[i])[0]));

        ret = (int)s->tx_stream->send(buff_ptrs, nsamps, s->tx_md);
      } else {
        ret = (int)s->tx_stream->send(&(((int16_t *)buff_tx[0])[0]), nsamps, s->tx_md);
      }

      if (ret != nsamps) {
        LOG_E(HW, "[xmit] tx samples %d != %d\n", ret, nsamps);
      }
      return ret;
    } else {
      pthread_mutex_lock(&write_thread->mutex_write);

      if (write_thread->count_write >= MAX_WRITE_THREAD_PACKAGE) {
        LOG_W(HW,
              "Buffer overflow, count_write = %d, start = %d end = %d, resetting write package\n",
              write_thread->count_write,
              write_thread->start,
              write_thread->end);
        write_thread->end = write_thread->start;
        write_thread->count_write = 0;
      }

      end = write_thread->end;
      write_package[end].timestamp = timestamp;
      write_package[end].nsamps = nsamps;
      write_package[end].cc = cc;
      write_package[end].first_packet = first_packet_state;
      write_package[end].last_packet = last_packet_state;
      write_package[end].flags_gpio = flags_gpio;
      for (int i = 0; i < cc; i++)
        write_package[end].buff[i] = buff[i];
      write_thread->count_write++;
      write_thread->end = (write_thread->end + 1) % MAX_WRITE_THREAD_PACKAGE;
      LOG_D(HW, "Signaling TX TS %llu\n", (unsigned long long)timestamp);
      pthread_cond_signal(&write_thread->cond_write);
      pthread_mutex_unlock(&write_thread->mutex_write);
      return nsamps;
    }
}

//-----------------------start--------------------------
/*! \brief Called to send samples to the USRP RF target
      @param device pointer to the device structure specific to the RF hardware target
      @param timestamp The timestamp at which the first sample MUST be sent
      @param buff Buffer which holds the samples
      @param nsamps number of samples to be sent
      @param antenna_id index of the antenna if the device has multiple antennas
      @param flags flags must be set to true if timestamp parameter needs to be applied
*/
void *trx_usrp_write_thread(void * arg)
{
  int ret=0;
  openair0_device_t *device=(openair0_device_t *)arg;
  openair0_thread_t *write_thread = &device->write_thread;
  openair0_write_package_t *write_package = write_thread->write_package;

  usrp_state_t *s;
  int nsamps2;  // aligned to upper 32 or 16 byte boundary
  int start;
  openair0_timestamp_t timestamp;
  void        **buff;
  int         nsamps;
  int         cc;
  signed char first_packet;
  signed char last_packet;
  int         flags_gpio;

  printf("trx_usrp_write_thread started on cpu %d\n",sched_getcpu());
  while(1){
    pthread_mutex_lock(&write_thread->mutex_write);
    while (write_thread->count_write == 0) {
      pthread_cond_wait(&write_thread->cond_write,&write_thread->mutex_write); // this unlocks mutex_rxtx while waiting and then locks it again
    }
    if (write_thread->write_thread_exit)
      break;
    VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME( VCD_SIGNAL_DUMPER_FUNCTIONS_TRX_WRITE_THREAD, 1 );
    s = (usrp_state_t *)device->priv;
    start = write_thread->start;
    timestamp    = write_package[start].timestamp;
    buff         = write_package[start].buff;
    nsamps       = write_package[start].nsamps;
    cc           = write_package[start].cc;
    first_packet = write_package[start].first_packet;
    last_packet  = write_package[start].last_packet;
    flags_gpio    = write_package[start].flags_gpio;
    write_thread->start = (write_thread->start + 1)% MAX_WRITE_THREAD_PACKAGE;
    write_thread->count_write--;
    pthread_mutex_unlock(&write_thread->mutex_write);
    /*if(write_thread->count_write != 0){
      LOG_W(HW,"count write = %d, start = %d, end = %d\n", write_thread->count_write, write_thread->start, write_thread->end);
    }*/

        nsamps2 = (nsamps+7)>>3;
        simde__m256i buff_tx[cc < 2 ? 2 : cc][nsamps2];
        // bring TX data into 16 MSBs, assuming it is on the 12 LSB  after OAI computation
        const int shift = 4;
        for (int i = 0; i < cc; i++) {
          for (int j = 0; j < nsamps2; j++) {
            if ((((uintptr_t) buff[i])&0x1F)==0) {
              buff_tx[i][j] = simde_mm256_slli_epi16(((simde__m256i *)buff[i])[j], shift);
            }
            else
            {
              simde__m256i tmp = simde_mm256_loadu_si256(((simde__m256i *)buff[i]) + j);
              buff_tx[i][j] = simde_mm256_slli_epi16(tmp, shift);
            }
          }
        }

    s->tx_md.has_time_spec  = true;
    s->tx_md.start_of_burst = (s->tx_count==0) ? true : first_packet;
    s->tx_md.end_of_burst   = last_packet;
    s->tx_md.time_spec      = uhd::time_spec_t::from_ticks(timestamp, s->sample_rate);
    LOG_D(PHY,"usrp_tx_write: tx_count %llu SoB %d, EoB %d, TS %llu\n",(unsigned long long)s->tx_count,s->tx_md.start_of_burst,s->tx_md.end_of_burst,(unsigned long long)timestamp); 
    s->tx_count++;

    // bit 3 enables gpio (for backward compatibility)
    if (flags_gpio&0x1000) {
      // push GPIO bits 
      s->usrp->set_command_time(s->tx_md.time_spec);
      s->usrp->set_gpio_attr(s->gpio_bank, "OUT", flags_gpio, MAN_MASK);
      s->usrp->clear_command_time();
    }

    if (cc>1) {
      std::vector<void *> buff_ptrs;

      for (int i=0; i<cc; i++)
        buff_ptrs.push_back(&(((int16_t *)buff_tx[i])[0]));

      ret = (int)s->tx_stream->send(buff_ptrs, nsamps, s->tx_md);
    }
    else {
      ret = (int)s->tx_stream->send(&(((int16_t *)buff_tx[0])[0]), nsamps, s->tx_md);
    }

    T(T_USRP_TX_ANT0, T_INT(timestamp), T_BUFFER(buff_tx[0], nsamps*4));

    if (ret != nsamps) LOG_E(HW,"[xmit] tx samples %d != %d\n",ret,nsamps);
    VCD_SIGNAL_DUMPER_DUMP_VARIABLE_BY_NAME( VCD_SIGNAL_DUMPER_VARIABLES_USRP_SEND_RETURN, ret );
    VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME( VCD_SIGNAL_DUMPER_FUNCTIONS_TRX_WRITE_THREAD, 0 );

  }

  return NULL;
}

int trx_usrp_write_init(openair0_device_t *device)
{
  //uhd::set_thread_priority_safe(1.0);
  openair0_thread_t *write_thread = &device->write_thread;
  printf("initializing tx write thread\n");

  write_thread->start              = 0;
  write_thread->end                = 0;
  write_thread->count_write        = 0;
  write_thread->write_thread_exit  = false;
  printf("end of tx write thread\n");
  pthread_mutex_init(&write_thread->mutex_write, NULL);
  pthread_cond_init(&write_thread->cond_write, NULL);
  threadCreate(&write_thread->pthread_write,
               trx_usrp_write_thread,
               (void *)device,
               (char*)"trx_usrp_write_thread",
               -1,
               OAI_PRIORITY_RT_MAX);
  return(0);
}

static void trx_usrp_write_reset(openair0_thread_t *wt) {
  pthread_mutex_lock(&wt->mutex_write);
  wt->count_write = 1;
  wt->write_thread_exit = true;
  pthread_cond_signal(&wt->cond_write);
  pthread_mutex_unlock(&wt->mutex_write);
  void *retval = NULL;
  pthread_join(wt->pthread_write, &retval);
  LOG_I(HW, "stopped USRP write thread\n");
}

//---------------------end-------------------------

/* Grow (never shrink) the three per-channel decimation scratch buffers to hold at least `nsamps`
 * (post-decimation, i.e. what the caller of trx_usrp_read() asked for) worth of samples at each
 * stage. Heap-allocated, plain malloc/free -- nothing downstream needs SIMD alignment (the raw
 * buffer is only ever handed to UHD's recv() as a generic byte buffer, and both decimation stages
 * are scalar loops), so there is no reason to pull in the codebase's malloc16 helper here. */
static void usrp_decim_ensure_scratch(usrp_state_t *s, int ch, int cc, int nsamps)
{
  const int raw_needed = nsamps * s->decim_ratio;
  if (s->decim_raw_buf_nsamps[ch] < raw_needed) {
    free(s->decim_raw_buf[ch]);
    s->decim_raw_buf[ch] = (int16_t *)malloc(sizeof(int16_t) * 2 * (size_t)raw_needed); // I/Q interleaved
    AssertFatal(s->decim_raw_buf[ch] != NULL, "USRP decim: cannot allocate %d raw samples for channel %d\n", raw_needed, ch);
    s->decim_raw_buf_nsamps[ch] = raw_needed;
  }

  // Channel-interleaved shared scratch, sized for ALL cc channels combined -- grown once; the
  // per-channel loop calling this just re-checks the same (already-satisfied) condition on later
  // iterations, which is cheap and keeps this function's per-channel calling convention unchanged.
  const int il_raw_needed = raw_needed * cc;
  if (s->decim_il_raw_nsamps < il_raw_needed) {
    free(s->decim_il_raw_re);
    free(s->decim_il_raw_im);
    s->decim_il_raw_re = (double *)malloc(sizeof(double) * (size_t)il_raw_needed);
    s->decim_il_raw_im = (double *)malloc(sizeof(double) * (size_t)il_raw_needed);
    AssertFatal(s->decim_il_raw_re != NULL && s->decim_il_raw_im != NULL,
                "USRP decim: cannot allocate interleaved raw scratch (%d samples)\n", il_raw_needed);
    s->decim_il_raw_nsamps = il_raw_needed;
  }
  const int il_stage1_needed = (raw_needed / 2) * cc;
  if (s->decim_il_stage1_nsamps < il_stage1_needed) {
    free(s->decim_il_stage1_re);
    free(s->decim_il_stage1_im);
    s->decim_il_stage1_re = (double *)malloc(sizeof(double) * (size_t)il_stage1_needed);
    s->decim_il_stage1_im = (double *)malloc(sizeof(double) * (size_t)il_stage1_needed);
    AssertFatal(s->decim_il_stage1_re != NULL && s->decim_il_stage1_im != NULL,
                "USRP decim: cannot allocate interleaved stage1 scratch (%d samples)\n", il_stage1_needed);
    s->decim_il_stage1_nsamps = il_stage1_needed;
  }
  const int il_stage2_needed = nsamps * cc;
  if (s->decim_il_stage2_nsamps < il_stage2_needed) {
    free(s->decim_il_stage2_re);
    free(s->decim_il_stage2_im);
    s->decim_il_stage2_re = (double *)malloc(sizeof(double) * (size_t)il_stage2_needed);
    s->decim_il_stage2_im = (double *)malloc(sizeof(double) * (size_t)il_stage2_needed);
    AssertFatal(s->decim_il_stage2_re != NULL && s->decim_il_stage2_im != NULL,
                "USRP decim: cannot allocate interleaved stage2 scratch (%d samples)\n", il_stage2_needed);
    s->decim_il_stage2_nsamps = il_stage2_needed;
  }
}

/*! \brief Receive samples from hardware.
 * Read \ref nsamps samples from each channel to buffers. buff[0] is the array for
 * the first channel. *ptimestamp is the time at which the first sample
 * was received.
 * \param device the hardware to use
 * \param[out] ptimestamp the time at which the first sample was received.
 * \param[out] buff An array of pointers to buffers for received samples. The buffers must be large enough to hold the number of samples \ref nsamps.
 * \param nsamps Number of samples. One sample is 2 byte I + 2 byte Q => 4 byte.
 * \param antenna_id Index of antenna for which to receive samples
 * \returns the number of sample read
*/
static int trx_usrp_read(openair0_device_t *device, openair0_timestamp_t *ptimestamp, void **buff, int nsamps, int cc)
{
  usrp_state_t *s = (usrp_state_t *)device->priv;
  int samples_received=0;
  int nsamps2; // aligned to upper 32 or 16 byte boundary
  nsamps2 = (nsamps+7)>>3;
  simde__m256i buff_tmp[cc < 2 ? 2 : cc][nsamps2];
  static int read_count = 0;
  int rxshift;
  switch (device->type) {
     case USRP_B200_DEV:
        rxshift=4;
        break;
     case USRP_X300_DEV:
     case USRP_N300_DEV:
     case USRP_X400_DEV:
        rxshift=2;
        break;
     default:
       AssertFatal(1==0,"Shouldn't be here\n");
  }

  // Software RX decimation (see the usrp_halfband_decimate2x block comment near the top of this
  // file): when active, we must pull decim_ratio times as many RAW samples from UHD as the caller
  // asked for, since this FPGA image can only stream at its one fixed native rate. decim==1 takes
  // the exact pre-existing code path below with zero behavioural change.
  const int decim = s->decim_ratio;
  const int recv_target = nsamps * decim;
  if (decim > 1) {
    for (int i = 0; i < cc; i++)
      usrp_decim_ensure_scratch(s, i, cc, nsamps);
  }

  samples_received=0;
  while (samples_received != recv_target) {

    if (cc>1) {
      // receive multiple channels (e.g. RF A and RF B)
      std::vector<void *> buff_ptrs;

      for (int i=0; i<cc; i++) {
        void *dst = (decim > 1) ? (void *)(s->decim_raw_buf[i] + 2 * samples_received)
                                 : (void *)((int32_t *)buff_tmp[i] + samples_received);
        buff_ptrs.push_back(dst);
      }
      samples_received += s->rx_stream->recv(buff_ptrs, recv_target-samples_received, s->rx_md);
    } else {
      // receive a single channel (e.g. from connector RF A)
      void *dst = (decim > 1) ? (void *)(s->decim_raw_buf[0] + 2 * samples_received)
                               : (void *)((int32_t *)buff_tmp[0] + samples_received);
      samples_received += s->rx_stream->recv(dst, recv_target-samples_received, s->rx_md);
    }
    if  ((s->wait_for_first_pps == 0) && (s->rx_md.error_code!=uhd::rx_metadata_t::ERROR_CODE_NONE))
      break;

    if ((s->wait_for_first_pps == 1) && (samples_received != recv_target)) {
      printf("sleep...\n"); //usleep(100);
    }
  }
  if (samples_received == recv_target) s->wait_for_first_pps=0;

  // How many LOGICAL (post-decimation) samples we actually have to hand downstream. Equals nsamps
  // on a full/normal receive; on a short read while decimating, only whole decimation windows of
  // raw samples correspond to a valid decimated output, so anything past that is not computed --
  // consistent with the pre-existing (non-decimating) short-read behaviour of simply reporting
  // fewer samples than asked for rather than fabricating data.
  int logical_received = samples_received;

  if (decim > 1) {
    if (samples_received == recv_target) {
      // TEMPORARY DIAGNOSTIC (2026-08-02): measure the decimation cascade's own wall-clock cost
      // directly, to settle whether remaining RX overflows are compute-bound (this taking too
      // long) or still a buffer/transport configuration issue, rather than guessing further.
      static int decim_timing_calls = 0;
      struct timespec ts_decim_start, ts_decim_end;
      clock_gettime(CLOCK_MONOTONIC, &ts_decim_start);
      {
        // Cross-channel SIMD interleaving (see usrp_decim_parallel_interleaved's block comment
        // for why the earlier per-channel-worker-split batching didn't help): stage 1's raw input
        // is transposed once into a channel-interleaved buffer so each stateless chunk job covers
        // all cc channels via one AVX FMA per tap; stage 2 reuses stage 1's own (already
        // interleaved) output directly, no second transpose.
        const int16_t *raw_re_arr[USRP_DECIM_MAX_CHANNELS];
        const int16_t *raw_im_arr[USRP_DECIM_MAX_CHANNELS];
        double *hist1_re_arr[USRP_DECIM_MAX_CHANNELS];
        double *hist1_im_arr[USRP_DECIM_MAX_CHANNELS];
        int *hist1_pos_arr[USRP_DECIM_MAX_CHANNELS];
        double *hist2_re_arr[USRP_DECIM_MAX_CHANNELS];
        double *hist2_im_arr[USRP_DECIM_MAX_CHANNELS];
        int *hist2_pos_arr[USRP_DECIM_MAX_CHANNELS];
        const double *stage1_view_re[USRP_DECIM_MAX_CHANNELS]; // strided view: channel c's own
        const double *stage1_view_im[USRP_DECIM_MAX_CHANNELS]; // column in the shared interleaved buffer

        const int n_workers = s->decim_num_workers;
        for (int i = 0; i < cc; i++) {
          raw_re_arr[i] = s->decim_raw_buf[i];
          raw_im_arr[i] = s->decim_raw_buf[i] + 1;
          hist1_re_arr[i] = s->decim_state[i].hist1_re;
          hist1_im_arr[i] = s->decim_state[i].hist1_im;
          hist1_pos_arr[i] = &s->decim_state[i].hist1_pos;
          hist2_re_arr[i] = s->decim_state[i].hist2_re;
          hist2_im_arr[i] = s->decim_state[i].hist2_im;
          hist2_pos_arr[i] = &s->decim_state[i].hist2_pos;
          stage1_view_re[i] = s->decim_il_stage1_re + i;
          stage1_view_im[i] = s->decim_il_stage1_im + i;
        }

        usrp_decim_transpose_channels<int16_t>(raw_re_arr, raw_im_arr, 2, cc, recv_target,
                                               s->decim_il_raw_re, s->decim_il_raw_im);
        usrp_decim_parallel_interleaved<int16_t>(s->decim_pool, n_workers,
                                                 usrp_decim_stage1_coefs, USRP_DECIM_STAGE1_TAPS,
                                                 hist1_re_arr, hist1_im_arr, hist1_pos_arr,
                                                 raw_re_arr, raw_im_arr, 2, cc, recv_target,
                                                 s->decim_il_raw_re, s->decim_il_raw_im,
                                                 s->decim_il_stage1_re, s->decim_il_stage1_im);
        usrp_decim_parallel_interleaved<double>(s->decim_pool, n_workers,
                                                usrp_decim_stage2_coefs, USRP_DECIM_STAGE2_TAPS,
                                                hist2_re_arr, hist2_im_arr, hist2_pos_arr,
                                                stage1_view_re, stage1_view_im, cc, cc, recv_target / 2,
                                                s->decim_il_stage1_re, s->decim_il_stage1_im,
                                                s->decim_il_stage2_re, s->decim_il_stage2_im);

        // Write the decimated output into buff_tmp in the SAME interleaved-int16 layout UHD's
        // recv() would have produced directly, so the existing rxshift loop below needs no
        // changes at all. The filter cascade has unity passband gain (verified numerically at
        // design time), so no additional scaling belongs here -- only rounding + a defensive
        // saturation clamp (a well-designed unity-gain filter on real ADC data should not clip,
        // but a silent int16 wraparound would be a far worse failure than a clean clip).
        for (int i = 0; i < cc; i++) {
          int16_t *dst = (int16_t *)buff_tmp[i];
          for (int j = 0; j < nsamps; j++) {
            long re_i = lround(s->decim_il_stage2_re[(size_t)j * cc + i]);
            long im_i = lround(s->decim_il_stage2_im[(size_t)j * cc + i]);
            if (re_i > 32767) re_i = 32767; else if (re_i < -32768) re_i = -32768;
            if (im_i > 32767) im_i = 32767; else if (im_i < -32768) im_i = -32768;
            dst[2 * j]     = (int16_t)re_i;
            dst[2 * j + 1] = (int16_t)im_i;
          }
        }
      }
      clock_gettime(CLOCK_MONOTONIC, &ts_decim_end);
      decim_timing_calls++;
      if (decim_timing_calls <= 20 || (decim_timing_calls % 200) == 0) {
        const double decim_us = (ts_decim_end.tv_sec - ts_decim_start.tv_sec) * 1e6
                               + (ts_decim_end.tv_nsec - ts_decim_start.tv_nsec) / 1e3;
        const double budget_us = 1e6 * (double)recv_target / (s->sample_rate * decim);
        LOG_W(HW, "DECIMTIMING call=%d decim_us=%.1f budget_us=%.1f (raw_samples=%d cc=%d)\n",
              decim_timing_calls, decim_us, budget_us, recv_target, cc);
      }
      logical_received = nsamps;
    } else {
      logical_received = samples_received / decim;
    }
  }

  // bring RX data into 12 LSBs for softmodem RX
  for (int i=0; i<cc; i++) {
    for (int j = 0; j < nsamps2; j++) {
      // bring RX data into 12 LSBs for softmodem RX,
      // this keeps the significant bits of B210 and may loose better ADC results,
      // but it makes free bits in MSB for signal processing on int16
      if ((((uintptr_t) buff[i])&0x1F)==0) {
        ((simde__m256i *)buff[i])[j] = simde_mm256_srai_epi16(buff_tmp[i][j], rxshift);
      } else {
        // FK: in some cases the buffer might not be 32 byte aligned, so we cannot use avx2
        simde__m256i tmp = simde_mm256_srai_epi16(buff_tmp[i][j], rxshift);
        simde_mm256_storeu_si256(((simde__m256i *)buff[i]) + j, tmp);
      }
    }
  }

  if (logical_received < nsamps) {
    LOG_E(HW,"[recv] received %d samples out of %d\n",logical_received,nsamps);
  }

  if ( s->rx_md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE)
    LOG_E(HW, "%s\n", s->rx_md.to_pp_string(true).c_str());

  s->rx_count += nsamps;
  s->rx_timestamp = s->rx_md.time_spec.to_ticks(s->sample_rate);
  *ptimestamp = s->rx_timestamp;

  /* ---- RAW RF TIMESTAMP DISCONTINUITY AUDIT (2026-08-06) --------------------------------------
   * Logged HERE, at the radio boundary, from the UHD metadata itself -- before the value reaches
   * any OAI diagnostic atomic -- so a genuine stream discontinuity can be told apart from a
   * conversion or wrap artifact downstream. Measured downstream: a single +641,449,058-sample
   * (~5.22 s) advance while OAI consumed its normal 2,457,612 samples, which destroys the
   * frame-to-sample mapping and is the actual cause of PBCH loss of lock.
   *
   * Deliberately event-driven with NO line cap: a discontinuity should be rare, and if it floods
   * that is itself the result. Every field needed to discriminate the three cases is printed
   * together -- raw previous/current timestamp, the expectation, requested vs returned samples,
   * and the UHD error/fragment flags. */
  {
    static_assert(sizeof(s->rx_timestamp) == 8, "Unexpected raw RF timestamp width");
    static uint64_t s_prev_raw_ts = 0;
    static int s_prev_returned = 0;
    static uint64_t s_disc_count = 0;
    const uint64_t cur = (uint64_t)s->rx_timestamp;
    if (s_prev_returned > 0) {
      const uint64_t expect = s_prev_raw_ts + (uint64_t)s_prev_returned;
      if (cur != expect) {
        s_disc_count++;
        LOG_E(HW,
              "SENSING: RFTSDISC n=%" PRIu64 " prev_raw=%" PRIu64 " cur_raw=%" PRIu64
              " expected=%" PRIu64 " delta=%+" PRId64 " requested=%d returned=%d prev_returned=%d "
              "uhd_err=%d(%s) more_fragments=%d has_time_spec=%d\n",
              s_disc_count, s_prev_raw_ts, cur, expect, (int64_t)(cur - expect), nsamps,
              logical_received, s_prev_returned, (int)s->rx_md.error_code,
              s->rx_md.strerror().c_str(), (int)s->rx_md.more_fragments,
              (int)s->rx_md.has_time_spec);
      }
    }
    s_prev_raw_ts = cur;
    s_prev_returned = (logical_received > 0) ? logical_received : nsamps;
  }

  T(T_USRP_RX_ANT0, T_INT(s->rx_timestamp), T_BUFFER(buff[0], logical_received*4));

  recplay_state_t *recPlay=device->recplay_state;

  if (device->openair0_cfg->recplay_mode == RECPLAY_RECORDMODE) { // record mode
    // Copy subframes to memory (later dump on a file)
    // The number of read samples might differ from BELL_LABS_IQ_BYTES_PER_SF
    // The number of read samples is always stored in nbBytes but the record is always of BELL_LABS_IQ_BYTES_PER_SF size
    if (recPlay->nbSamplesBlocks <= device->openair0_cfg->recplay_conf->u_sf_max &&
        recPlay->maxSizeBytes >= (recPlay->currentPtr-(uint8_t *)recPlay->ms_sample) +
        sizeof(iqrec_t) + BELL_LABS_IQ_BYTES_PER_SF) {
      iqrec_t *hdr=(iqrec_t *)recPlay->currentPtr;
      struct timespec trec;
      (void) clock_gettime(CLOCK_REALTIME, &trec);
      hdr->header = BELL_LABS_IQ_HEADER;
      hdr->ts = *ptimestamp;
      hdr->nbBytes=nsamps*4;            // real number of samples bytes
      hdr->tv_sec = trec.tv_sec;        // record secs
      hdr->tv_usec = trec.tv_nsec/1000; // record µsecs
      memcpy(hdr+1, buff[0], nsamps*4);
      recPlay->currentPtr+=sizeof(iqrec_t)+BELL_LABS_IQ_BYTES_PER_SF; // record size is constant (BELL_LABS_IQ_BYTES_PER_SF)
      recPlay->nbSamplesBlocks++;
      LOG_D(HW,"recorded %d samples, for TS %lu, shift in buffer %ld nbBytes %d nbSamplesBlocks %d\n", nsamps, hdr->ts, recPlay->currentPtr-(uint8_t *)recPlay->ms_sample, (int)hdr->nbBytes, (int)recPlay->nbSamplesBlocks);
    } else
      exit_function(__FILE__, __FUNCTION__, __LINE__, "Recording reaches max iq limit\n", OAI_EXIT_NORMAL);
  }
  read_count++;
  LOG_D(HW,"usrp_lib: returning %d samples at ts %lu read_count %d\n", logical_received, *ptimestamp, read_count);
  return logical_received;
}

/*! \brief Compares two variables within precision
 * \param a first variable
 * \param b second variable
*/
static bool is_equal(double a, double b) {
  return std::fabs(a-b) < std::numeric_limits<double>::epsilon();
}

void *freq_thread(void *arg) {
  openair0_device_t *device=(openair0_device_t *)arg;
  usrp_state_t *s = (usrp_state_t *)device->priv;
  uhd::tune_request_t tx_tune_req(device->openair0_cfg[0].tx_freq[0],
                                  device->openair0_cfg[0].tune_offset);
  uhd::tune_request_t rx_tune_req(device->openair0_cfg[0].rx_freq[0],
                                  device->openair0_cfg[0].tune_offset);
  s->usrp->set_tx_freq(tx_tune_req);
  s->usrp->set_rx_freq(rx_tune_req);
  return NULL;
}
/*! \brief Set frequencies (TX/RX). Spawns a thread to handle the frequency change to not block the calling thread
 * \param device the hardware to use
 * \param openair0_cfg RF frontend parameters set by application
 * \param dummy dummy variable not used
 * \returns 0 in success
 */
int trx_usrp_set_freq(openair0_device_t *device, openair0_config_t *openair0_cfg)
{
  usrp_state_t *s = (usrp_state_t *)device->priv;
  printf("Setting USRP TX Freq %f, RX Freq %f, tune_offset: %f\n", openair0_cfg[0].tx_freq[0], openair0_cfg[0].rx_freq[0], openair0_cfg[0].tune_offset);

  uhd::tune_request_t tx_tune_req(openair0_cfg[0].tx_freq[0], openair0_cfg[0].tune_offset);
  uhd::tune_request_t rx_tune_req(openair0_cfg[0].rx_freq[0], openair0_cfg[0].tune_offset);
  s->usrp->set_tx_freq(tx_tune_req);
  s->usrp->set_rx_freq(rx_tune_req);

  return(0);
}

/*! \brief Set RX frequencies
 * \param device the hardware to use
 * \param openair0_cfg RF frontend parameters set by application
 * \returns 0 in success
 */
int openair0_set_rx_frequencies(openair0_device_t *device, openair0_config_t *openair0_cfg)
{
  usrp_state_t *s = (usrp_state_t *)device->priv;
  uhd::tune_request_t rx_tune_req(openair0_cfg[0].rx_freq[0], openair0_cfg[0].tune_offset);
  printf("In openair0_set_rx_frequencies, freq: %f, tune offset: %f\n",
         openair0_cfg[0].rx_freq[0],  openair0_cfg[0].tune_offset);
  //rx_tune_req.rf_freq_policy = uhd::tune_request_t::POLICY_MANUAL;
  //rx_tune_req.rf_freq = openair0_cfg[0].rx_freq[0];
  s->usrp->set_rx_freq(rx_tune_req);
  return(0);
}

/*! \brief Set Gains (TX/RX)
 * \param device the hardware to use
 * \param openair0_cfg RF frontend parameters set by application
 * \returns 0 in success
 */
int trx_usrp_set_gains(openair0_device_t *device,
                       openair0_config_t *openair0_cfg)
{
  usrp_state_t *s = (usrp_state_t *)device->priv;
  ::uhd::gain_range_t gain_range_tx = s->usrp->get_tx_gain_range(0);
  s->usrp->set_tx_gain(gain_range_tx.stop()-openair0_cfg[0].tx_gain[0]);
  ::uhd::gain_range_t gain_range = s->usrp->get_rx_gain_range(0);

  // limit to maximum gain
  if (openair0_cfg[0].rx_gain[0]-openair0_cfg[0].rx_gain_offset[0] > gain_range.stop()) {
    LOG_E(HW,"RX Gain 0 too high, reduce by %f dB\n",
          openair0_cfg[0].rx_gain[0]-openair0_cfg[0].rx_gain_offset[0] - gain_range.stop());
    int gain_diff = gain_range.stop() - (openair0_cfg[0].rx_gain[0] - openair0_cfg[0].rx_gain_offset[0]);
    return gain_diff;
  }

  s->usrp->set_rx_gain(openair0_cfg[0].rx_gain[0]-openair0_cfg[0].rx_gain_offset[0]);
  LOG_I(HW,"Setting USRP RX gain to %f (rx_gain %f,gain_range.stop() %f)\n",
        openair0_cfg[0].rx_gain[0]-openair0_cfg[0].rx_gain_offset[0],
        openair0_cfg[0].rx_gain[0],gain_range.stop());
  return(0);
}

/*! \brief Stop USRP
 * \param card refers to the hardware index to use
 */
int trx_usrp_stop(openair0_device_t *device)
{
  UNUSED(device);
  return(0);
}

/*! \brief USRPB210 RX calibration table */
rx_gain_calib_table_t calib_table_b210[] = {
  {3500000000.0,44.0},
  {2660000000.0,49.0},
  {2300000000.0,50.0},
  {1880000000.0,53.0},
  {816000000.0,58.0},
  {-1,0}
};

/*! \brief USRPB210 RX calibration table */
rx_gain_calib_table_t calib_table_b210_38[] = {
  {3500000000.0,44.0},
  {2660000000.0,49.8},
  {2300000000.0,51.0},
  {1880000000.0,53.0},
  {816000000.0,57.0},
  {-1,0}
};

/*! \brief USRPx310 RX calibration table */
rx_gain_calib_table_t calib_table_x310[] = {
  {3500000000.0,77.0},
  {2660000000.0,81.0},
  {2300000000.0,81.0},
  {1880000000.0,82.0},
  {816000000.0,85.0},
  {-1,0}
};

/*! \brief USRPn3xf RX calibration table */
rx_gain_calib_table_t calib_table_n310[] = {
  {3500000000.0,0.0},
  {2660000000.0,0.0},
  {2300000000.0,0.0},
  {1880000000.0,0.0},
  {816000000.0, 0.0},
  {-1,0}
};

/*! \brief Empty RX calibration table */
rx_gain_calib_table_t calib_table_none[] = {
  {3500000000.0,0.0},
  {2660000000.0,0.0},
  {2300000000.0,0.0},
  {1880000000.0,0.0},
  {816000000.0, 0.0},
  {-1,0}
};


/*! \brief Set RX gain offset
 * \param openair0_cfg RF frontend parameters set by application
 * \param chain_index RF chain to apply settings to
 * \returns 0 in success
 */
void set_rx_gain_offset(openair0_config_t *openair0_cfg, int chain_index,int bw_gain_adjust) {
  int i=0;
  // loop through calibration table to find best adjustment factor for RX frequency
  double min_diff = 6e9,diff,gain_adj=0.0;

  if (bw_gain_adjust==1) {
    switch ((int)openair0_cfg[0].sample_rate) {
      case 46080000:
        break;

      case 30720000:
        break;

      case 23040000:
        gain_adj=1.25;
        break;

      case 15360000:
        gain_adj=3.0;
        break;

      case 7680000:
        gain_adj=6.0;
        break;

      case 3840000:
        gain_adj=9.0;
        break;

      case 1920000:
        gain_adj=12.0;
        break;

      default:
        LOG_E(HW,"unknown sampling rate %d\n",(int)openair0_cfg[0].sample_rate);
        //exit(-1);
        break;
    }
  }

  while (openair0_cfg->rx_gain_calib_table[i].freq>0) {
    diff = fabs(openair0_cfg->rx_freq[chain_index] - openair0_cfg->rx_gain_calib_table[i].freq);
    LOG_I(HW,"cal %d: freq %f, offset %f, diff %f\n",
          i,
          openair0_cfg->rx_gain_calib_table[i].freq,
          openair0_cfg->rx_gain_calib_table[i].offset,diff);

    if (min_diff > diff) {
      min_diff = diff;
      openair0_cfg->rx_gain_offset[chain_index] = openair0_cfg->rx_gain_calib_table[i].offset+gain_adj;
    }

    i++;
  }
}

/*! \brief print the USRP statistics
* \param device the hardware to use
* \returns  0 on success
*/
int trx_usrp_get_stats(openair0_device_t *device)
{
  UNUSED(device);
  return(0);
}

/*! \brief Reset the USRP statistics
 * \param device the hardware to use
 * \returns  0 on success
 */
int trx_usrp_reset_stats(openair0_device_t *device)
{
  UNUSED(device);
  return(0);
}

/*! \brief synch the USRP time accross devices using the host clock (ideally syched with PTP, but also NTP works) and assuming all
 * devices are synched by octoclock */
static void usrp_sync_pps(usrp_state_t *s)
{
  // First, wait for PPS.
  uhd::time_spec_t time_last_pps = s->usrp->get_time_last_pps();

  while (time_last_pps == s->usrp->get_time_last_pps()) {
    boost::this_thread::sleep(boost::posix_time::milliseconds(1));
  }

  // get host time
  struct timespec tp;
  if (clock_gettime(CLOCK_TAI, &tp) != 0)
    LOG_W(PHY, "error getting system time\n");
  double tai_sec = (double)tp.tv_sec;

  // set USRP time to host time at next pps
  s->usrp->set_time_next_pps(uhd::time_spec_t(tai_sec));
  LOG_I(HW, "USRP clock set to %f sec\n", tai_sec);
}

extern "C" {
  int device_init(openair0_device_t *device, openair0_config_t *openair0_cfg)
  {
    LOG_I(HW, "openair0_cfg[0].sdr_addrs == '%s'\n", openair0_cfg[0].sdr_addrs);
    LOG_I(HW, "openair0_cfg[0].clock_source == '%d' (internal = %d, external = %d)\n", openair0_cfg[0].clock_source,internal,external);
    usrp_state_t *s ;
    int choffset = 0;

    if ( device->priv == NULL) {
      s=(usrp_state_t *)calloc(1, sizeof(usrp_state_t));
      device->priv=s;
      AssertFatal( s!=NULL,"USRP device: memory allocation failure\n");
    } else {
      LOG_E(HW, "multiple device init detected\n");
      return 0;
    }

    device->openair0_cfg = openair0_cfg;
    device->trx_start_func = trx_usrp_start;
    device->trx_get_stats_func = trx_usrp_get_stats;
    device->trx_reset_stats_func = trx_usrp_reset_stats;
    device->trx_end_func   = trx_usrp_end;
    device->trx_stop_func  = trx_usrp_stop;
    device->trx_set_freq_func = trx_usrp_set_freq;
    device->trx_set_gains_func   = trx_usrp_set_gains;
    device->trx_write_init = trx_usrp_write_init;


    // hotfix! to be checked later
    //uhd::set_thread_priority_safe(1.0);
    // Initialize USRP device
    int vers=0,subvers=0,subsubvers=0;
    int bw_gain_adjust=0;

    if (device->openair0_cfg->recplay_mode == RECPLAY_RECORDMODE) {
      std::cerr << "USRP device initialized in subframes record mode" << std::endl;
    }

    sscanf(uhd::get_version_string().c_str(),"%d.%d.%d",&vers,&subvers,&subsubvers);
    LOG_I(HW,"UHD version %s (%d.%d.%d)\n",
          uhd::get_version_string().c_str(),vers,subvers,subsubvers);
    std::string args,tx_subdev,rx_subdev;

    if (openair0_cfg[0].sdr_addrs == NULL) {
      args = "type=b200";
    } else {
      args = openair0_cfg[0].sdr_addrs;
      LOG_I(HW,"Checking for USRP with args %s\n",openair0_cfg[0].sdr_addrs);
    }

    uhd::device_addrs_t device_adds = uhd::device::find(args);

    if (device_adds.size() == 0) {
      LOG_E(HW,"No USRP Device Found.\n ");
      free(s);
      return -1;
    } else if (device_adds.size() > 1) {
      LOG_E(HW,"More than one USRP Device Found. Please specify device more precisely in config file.\n");
      free(s);
      return -1;
    }

    std::string type_str, product_str;
    if (args.find("addr0") != std::string::npos) {
      type_str = "type0";
      product_str = "product0";
    }
    else {
      type_str = "type";
      product_str = "product";
    }
    
    LOG_I(HW,"Found USRP %s\n", device_adds[0].get(type_str).c_str());
    double usrp_master_clock;

    if (device_adds[0].get(type_str) == "b200") {
      device->type = USRP_B200_DEV;
      usrp_master_clock = 30.72e6;
      args += boost::str(boost::format(",master_clock_rate=%f") % usrp_master_clock);
      args += ",num_send_frames=256,num_recv_frames=256, send_frame_size=7680, recv_frame_size=7680" ;
    }

    if (device_adds[0].get(type_str) == "n3xx") {
      const std::string product = device_adds[0].get(product_str);
      printf("Found USRP %s\n", product.c_str());
      device->type=USRP_N300_DEV;
      if (product == "n320")
        usrp_master_clock = 245.76e6; // N320 does not support 122.88e6 master clock rate
      else
        usrp_master_clock = 122.88e6;
      args += boost::str(boost::format(",master_clock_rate=%f") % usrp_master_clock);

      if ( 0 != system("sysctl -w net.core.rmem_max=62500000 net.core.wmem_max=62500000") )
        LOG_W(HW,"Can't set kernel parameters for N3x0\n");
    }

    if (device_adds[0].get(type_str) == "x300") {
      printf("Found USRP x300\n");
      device->type=USRP_X300_DEV;
      usrp_master_clock = 184.32e6;
      args += boost::str(boost::format(",master_clock_rate=%f") % usrp_master_clock);

      // USRP recommended: https://files.ettus.com/manual/page_usrp_x3x0_config.html
      if ( 0 != system("sysctl -w net.core.rmem_max=33554432 net.core.wmem_max=33554432") )
        LOG_W(HW,"Can't set kernel parameters for X3xx\n");
    }

    if (device_adds[0].get(type_str) == "x4xx") {
      printf("Found USRP x400\n");
      device->type = USRP_X400_DEV;
      // 245.76 MHz was previously hardcoded here for every X4xx configuration, an assumption that
      // only holds for FPGA images with a DDC/decimation block (e.g. the default "X4_200" image).
      // MEASURED (2026-08-02): the "CG" (100 GbE dual-QSFP28) image has no DDC block at all --
      // confirmed via `rfnoc_graph::find_blocks("DDC")` returning empty -- and its ONLY valid
      // master clock rate is its native ADC rate, 491.52 MHz; requesting 245.76 MHz against it
      // fails outright at MPM's init() RPC, before any sample-rate negotiation is even reached.
      // The currently loaded image is already reported by discovery (the same `fpga=...` string
      // visible in the MPMD init log), so branch on it instead of assuming one MCR for all X4xx.
      const std::string fpga_type = device_adds[0].get("fpga", "");
      if (fpga_type.rfind("CG", 0) == 0) { // "CG_400", or any future CG-prefixed variant
        usrp_master_clock = 491.52e6;
        LOG_W(HW,
              "X4xx FPGA image type '%s' has no DDC block -- native rate %.2f MHz is fixed; "
              "requested sample rates that don't match it will need software decimation (see the "
              "usrp_halfband_decimate2x block comment near the top of this file)\n",
              fpga_type.c_str(), usrp_master_clock / 1e6);
      } else {
        usrp_master_clock = 245.76e6;
      }
      args += boost::str(boost::format(",master_clock_rate=%f") % usrp_master_clock);

      // https://kb.ettus.com/USRP_Host_Performance_Tuning_Tips_and_Tricks
      //
      // 62.5 MB was sized for the 10 GbE / 245.76 MSps X4_200 case. MEASURED (2026-08-02): at
      // 491.52 MSps over 100 GbE (the CG_400 image) this is only 25% of what UHD itself asks for
      // -- every run logs "Target sock buff size: 250000000 bytes. Actual sock buff size:
      // 62500000 bytes" -- and RX overflows appeared (0 -> 140+/90s) that were never present at
      // the lower rate. Matching UHD's own requested size rather than guessing a bigger one.
      if (0 != system("sysctl -w net.core.rmem_max=250000000 net.core.wmem_max=250000000"))
        LOG_W(HW, "Can't set kernel parameters for X4x0\n");
    }

    s->usrp = uhd::usrp::multi_usrp::make(args);

  if (device->type==USRP_X300_DEV) {
    openair0_cfg[0].rx_gain_calib_table = calib_table_x310;
    std::cerr << "-- Using calibration table: calib_table_x310" << std::endl;
  }

  if (device->type==USRP_N300_DEV) {
    openair0_cfg[0].rx_gain_calib_table = calib_table_n310;
    std::cerr << "-- Using calibration table: calib_table_n310" << std::endl;
  }

  if (device->type == USRP_X400_DEV) {
    openair0_cfg[0].rx_gain_calib_table = calib_table_none;
    std::cerr << "-- Using calibration table: calib_table_none" << std::endl;
  }


  if (device->type==USRP_N300_DEV || device->type==USRP_X300_DEV || device->type==USRP_X400_DEV) {
    LOG_I(HW,"%s() sample_rate:%u\n", __FUNCTION__, (int)openair0_cfg[0].sample_rate);

    switch ((int)openair0_cfg[0].sample_rate) {
      case 245760000:
        // from usrp_time_offset
        // openair0_cfg[0].samples_per_packet    = 2048;
        openair0_cfg[0].tx_sample_advance = 15; // to be checked
        openair0_cfg[0].tx_bw = 200e6;
        openair0_cfg[0].rx_bw = 200e6;
        break;

      case 184320000:
        // from usrp_time_offset
        // openair0_cfg[0].samples_per_packet    = 2048;
        openair0_cfg[0].tx_sample_advance = 15; // to be checked
        openair0_cfg[0].tx_bw = 100e6;
        openair0_cfg[0].rx_bw = 100e6;
        break;

      case 122880000:
        // from usrp_time_offset
        //openair0_cfg[0].samples_per_packet    = 2048;
        openair0_cfg[0].tx_sample_advance     = 15; //to be checked
        // 100e6, not the former 80e6: 122.88 Msps carries a 273-PRB/100 MHz carrier, which spans
        // 273*12*30kHz = 98.28 MHz. An 80 MHz analog filter centred on the carrier truncates ~9 MHz
        // at EACH edge, and an SSB placed low in the carrier then falls outside the passband
        // entirely -- measured on a live srsRAN cell whose SSB sat 40 MHz below carrier centre
        // (3374.4 MHz vs 3414.99 MHz): the filter's lower edge landed at 3374.99 MHz, 0.6 MHz above
        // the SSB, and initial sync could never see it. Widening to 100e6 covers the whole carrier.
        openair0_cfg[0].tx_bw                 = 100e6;
        openair0_cfg[0].rx_bw                 = 100e6;
        break;

      case 92160000:
        // from usrp_time_offset
        //openair0_cfg[0].samples_per_packet    = 2048;
        openair0_cfg[0].tx_sample_advance     = 15; //to be checked
        //openair0_cfg[0].tx_bw                 = 80e6;
        //openair0_cfg[0].rx_bw                 = 80e6;
        break;

      case 61440000:
        // from usrp_time_offset
        //openair0_cfg[0].samples_per_packet    = 2048;
        openair0_cfg[0].tx_sample_advance     = 15;
        openair0_cfg[0].tx_bw                 = 40e6;
        openair0_cfg[0].rx_bw                 = 40e6;
        break;

      case 46080000:
        //openair0_cfg[0].samples_per_packet    = 2048;
        openair0_cfg[0].tx_sample_advance     = 15;
        openair0_cfg[0].tx_bw                 = 40e6;
        openair0_cfg[0].rx_bw                 = 40e6;
        break;

      case 30720000:
        // from usrp_time_offset
        //openair0_cfg[0].samples_per_packet    = 2048;
        openair0_cfg[0].tx_sample_advance     = 15;
        openair0_cfg[0].tx_bw                 = 20e6;
        openair0_cfg[0].rx_bw                 = 20e6;
        break;

      case 23040000:
        //openair0_cfg[0].samples_per_packet    = 2048;
        openair0_cfg[0].tx_sample_advance     = 15;
        openair0_cfg[0].tx_bw                 = 20e6;
        openair0_cfg[0].rx_bw                 = 20e6;
        break;

      case 15360000:
        //openair0_cfg[0].samples_per_packet    = 2048;
        openair0_cfg[0].tx_sample_advance     = 45;
        openair0_cfg[0].tx_bw                 = 10e6;
        openair0_cfg[0].rx_bw                 = 10e6;
        break;

      case 7680000:
        //openair0_cfg[0].samples_per_packet    = 2048;
        openair0_cfg[0].tx_sample_advance     = 50;
        openair0_cfg[0].tx_bw                 = 5e6;
        openair0_cfg[0].rx_bw                 = 5e6;
        break;

      case 1920000:
        //openair0_cfg[0].samples_per_packet    = 2048;
        openair0_cfg[0].tx_sample_advance     = 50;
        openair0_cfg[0].tx_bw                 = 1.25e6;
        openair0_cfg[0].rx_bw                 = 1.25e6;
        break;

      default:
        LOG_E(HW,"Error: unknown sampling rate %f\n",openair0_cfg[0].sample_rate);
        exit(-1);
        break;
    }
  }

  if (device->type == USRP_B200_DEV) {
    if ((vers == 3) && (subvers == 9) && (subsubvers>=2)) {
      openair0_cfg[0].rx_gain_calib_table = calib_table_b210;
      bw_gain_adjust=0;
      std::cerr << "-- Using calibration table: calib_table_b210" << std::endl; // Bell Labs info
    } else {
      openair0_cfg[0].rx_gain_calib_table = calib_table_b210_38;
      bw_gain_adjust=1;
      std::cerr << "-- Using calibration table: calib_table_b210_38" << std::endl; // Bell Labs info
    }

    switch ((int)openair0_cfg[0].sample_rate) {
      case 46080000:
        s->usrp->set_master_clock_rate(46.08e6);
        //openair0_cfg[0].samples_per_packet    = 1024;
        openair0_cfg[0].tx_sample_advance     = 164;
        openair0_cfg[0].tx_bw                 = 40e6;
        openair0_cfg[0].rx_bw                 = 40e6;
        break;

      case 30720000:
        s->usrp->set_master_clock_rate(30.72e6);
        //openair0_cfg[0].samples_per_packet    = 1024;
        openair0_cfg[0].tx_sample_advance     = 115;
        openair0_cfg[0].tx_bw                 = 20e6;
        openair0_cfg[0].rx_bw                 = 20e6;
        break;

      case 23040000:
        s->usrp->set_master_clock_rate(23.04e6); //to be checked
        //openair0_cfg[0].samples_per_packet    = 1024;
        openair0_cfg[0].tx_sample_advance     = 113;
        openair0_cfg[0].tx_bw                 = 20e6;
        openair0_cfg[0].rx_bw                 = 20e6;
        break;

      case 15360000:
        s->usrp->set_master_clock_rate(30.72e06);
        //openair0_cfg[0].samples_per_packet    = 1024;
        openair0_cfg[0].tx_sample_advance     = 103;
        openair0_cfg[0].tx_bw                 = 20e6;
        openair0_cfg[0].rx_bw                 = 20e6;
        break;

      case 7680000:
        s->usrp->set_master_clock_rate(30.72e6);
        //openair0_cfg[0].samples_per_packet    = 1024;
        openair0_cfg[0].tx_sample_advance     = 80;
        openair0_cfg[0].tx_bw                 = 20e6;
        openair0_cfg[0].rx_bw                 = 20e6;
        break;

      case 1920000:
        s->usrp->set_master_clock_rate(30.72e6);
        //openair0_cfg[0].samples_per_packet    = 1024;
        openair0_cfg[0].tx_sample_advance     = 40;
        openair0_cfg[0].tx_bw                 = 20e6;
        openair0_cfg[0].rx_bw                 = 20e6;
        break;

      default:
        LOG_E(HW,"Error: unknown sampling rate %f\n",openair0_cfg[0].sample_rate);
        exit(-1);
        break;
    }
  }

  if(openair0_cfg[0].tx_subdev!=NULL){
    LOG_I(HW, "openair0_cfg[0].tx_subdev == %s\n", openair0_cfg[0].tx_subdev);
    tx_subdev = openair0_cfg[0].tx_subdev;
    s->usrp->set_tx_subdev_spec(tx_subdev);
  }

  if(openair0_cfg[0].rx_subdev!=NULL){
    LOG_I(HW, "openair0_cfg[0].rx_subdev == %s\n", openair0_cfg[0].rx_subdev);
    rx_subdev = openair0_cfg[0].rx_subdev;
    s->usrp->set_rx_subdev_spec(rx_subdev);
  }

  for(int i=0; i<((int) s->usrp->get_rx_num_channels()); i++) {
    openair0_config_t *cfg = &openair0_cfg[0];
    if (i < cfg->rx_num_channels) {
      s->usrp->set_rx_rate(cfg->sample_rate, i + choffset);
      // Some FPGA images have no DDC/decimation block and silently coerce the granted rate to a
      // fixed native ADC rate regardless of what was requested (measured: X410 "CG" 100 GbE image,
      // 491.52 MSps native vs the 122.88 MSps a 273 PRB/100 MHz NR capture needs -- see the
      // usrp_halfband_decimate2x block comment near the top of this file for the full story).
      // Detected generically from the requested-vs-granted ratio so this isn't X410/CG-specific.
      {
        const double granted = s->usrp->get_rx_rate(i + choffset);
        const double ratio_f = granted / cfg->sample_rate;
        const int ratio = (int)llround(ratio_f);
        const bool ratio_is_clean = fabs(ratio_f - (double)ratio) < 1e-3;
        const bool ratio_is_supported = (ratio == 1 || ratio == 4);
        AssertFatal(ratio_is_clean && ratio_is_supported,
                    "USRP granted RX rate %f Hz is not the requested %f Hz (ratio %f) and is not a "
                    "supported software-decimation factor (only 1x/4x are implemented) -- refusing "
                    "to stream a rate OAI's timing model does not expect\n",
                    granted, cfg->sample_rate, ratio_f);
        if (i == 0) { // ratio is a device-wide property (shared ADC clock), not per-channel
          s->decim_ratio = (ratio == 4) ? 4 : 1;
          if (s->decim_ratio > 1) {
            LOG_W(HW,
                  "USRP granted RX rate %.0f Hz != requested %.0f Hz -- enabling %dx software RX "
                  "decimation (no DDC block in this FPGA image)\n",
                  granted, cfg->sample_rate, s->decim_ratio);
            // hw_cores - 2 leaves headroom for the softmodem's own real-time threads (UE_thread,
            // DL/UL actors, sensing engine consumer, ...). MEASURED (2026-08-02, spin-wait pool,
            // sens3's 8-core Xeon W-2225): P=6 gave 9.58x/2.96x speedup (stage1/stage2) and was
            // the best of {2,4,6,8}; P=8 measured WORSE (2.78x/1.34x) -- with spin-waiting workers
            // burning cycles even while idle, using ALL physical cores for the pool starves the
            // softmodem's own threads and everything slows down net. hw_cores-2 already lands on
            // exactly 6 for this 8-core box, which is why the formula (not just the cap) matters.
            unsigned hw_cores = std::thread::hardware_concurrency();
            if (hw_cores == 0)
              hw_cores = 4; // std::thread::hardware_concurrency() is allowed to return 0 if undetectable
            int p = (int)hw_cores - 2;
            if (p < 2) p = 2;
            if (p > 8) p = 8; // untested above 8; the P=8-is-worse result above suggests this cap is generous, not tight
            // TEMPORARY (2026-08-02): env override for re-tuning P against LIVE contention (the
            // isolated-stage offline benchmark that picked hw_cores-2 did not, and could not,
            // account for spin-waiting workers competing with the softmodem's OWN real-time
            // threads for the same physical cores -- that only shows up live). Remove once P is
            // re-settled against a live measurement instead of the isolated offline one.
            const char *p_override = getenv("USRP_DECIM_WORKERS");
            if (p_override != NULL) {
              int p_env = atoi(p_override);
              if (p_env >= 1 && p_env <= USRP_DECIM_SPINPOOL_MAX_WORKERS) {
                p = p_env;
                LOG_W(HW, "USRP decim: worker count overridden to %d via USRP_DECIM_WORKERS\n", p);
              }
            }
            s->decim_num_workers = p;
            s->decim_pool = new UsrpDecimWorkerPool(p);
            LOG_I(HW, "USRP decim: started %d-worker pool for parallel software decimation (hw_cores=%u)\n", p, hw_cores);
          }
        }
      }
      uhd::tune_request_t rx_tune_req(cfg->rx_freq[i], cfg->tune_offset);
      s->usrp->set_rx_freq(rx_tune_req, i+choffset);
      set_rx_gain_offset(cfg, i, bw_gain_adjust);
      ::uhd::gain_range_t gain_range = s->usrp->get_rx_gain_range(i+choffset);
      // limit to maximum gain
      double gain = cfg->rx_gain[i] - cfg->rx_gain_offset[i];
      if ( gain > gain_range.stop())  {
        LOG_E(HW, "RX Gain too high, lower by %f dB\n", gain - gain_range.stop());
        gain = gain_range.stop();
      }
      s->usrp->set_rx_gain(gain,i+choffset);
      LOG_I(HW,
            "RX Gain %d %f (%f) => %f (max %f)\n",
            i,
            cfg->rx_gain[i],
            cfg->rx_gain_offset[i],
            cfg->rx_gain[i] - cfg->rx_gain_offset[i],
            gain_range.stop());
      // Bistatic sensing UE: force RX onto a connector physically separate from the one TX uses,
      // improving TX->RX isolation. OAI otherwise leaves the antenna at the UHD default, which
      // shares one port for both directions on a B2x0.
      //
      // PORT NAMES ARE DEVICE-SPECIFIC and an invalid name is not a soft failure -- it segfaults
      // inside libuhd (null deref). Measured on an X410 (UHD 4.10): its ports are
      // {TX/RX0, RX1, CAL_LOOPBACK, TERMINATION} for RX and {TX/RX0, CAL_LOOPBACK} for TX, so the
      // B2x0 names "RX2"/"TX/RX" -- which this block previously applied UNCONDITIONALLY to every
      // device type -- crash the X410 before it ever streams. Hence the explicit per-type switch,
      // and no forcing at all on device types whose port naming hasn't been verified here.
      const char *rx_ant = NULL;
      if (device->type == USRP_B200_DEV) {
        rx_ant = "RX2";
      } else if (device->type == USRP_X400_DEV) {
        rx_ant = "RX1"; // X410: TX stays on TX/RX0, so RX1 is the separate-connector choice
      }
      if (rx_ant != NULL) {
        s->usrp->set_rx_antenna(rx_ant, i + choffset);
        LOG_I(HW, "RX antenna forced to %s on channel %d\n",
              s->usrp->get_rx_antenna(i + choffset).c_str(), i);
      } else {
        LOG_I(HW, "RX antenna left at UHD default (%s) on channel %d -- port naming not verified "
                  "for this device type\n",
              s->usrp->get_rx_antenna(i + choffset).c_str(), i);
      }
    }
  }

  LOG_D(HW, "usrp->get_tx_num_channels() == %zd\n", s->usrp->get_tx_num_channels());
  LOG_D(HW, "openair0_cfg[0].tx_num_channels == %d\n", openair0_cfg[0].tx_num_channels);

  for(int i=0; i<((int) s->usrp->get_tx_num_channels()); i++) {
    ::uhd::gain_range_t gain_range_tx = s->usrp->get_tx_gain_range(i);

    if (i<openair0_cfg[0].tx_num_channels) {
      s->usrp->set_tx_rate(openair0_cfg[0].sample_rate,i+choffset);
      uhd::tune_request_t tx_tune_req(openair0_cfg[0].tx_freq[i],
                                      openair0_cfg[0].tune_offset);
      s->usrp->set_tx_freq(tx_tune_req, i+choffset);
      s->usrp->set_tx_gain(gain_range_tx.stop()-openair0_cfg[0].tx_gain[i],i+choffset);
      LOG_I(HW,"USRP TX_GAIN:%3.2lf gain_range:%3.2lf tx_gain:%3.2lf\n", gain_range_tx.stop()-openair0_cfg[0].tx_gain[i], gain_range_tx.stop(), openair0_cfg[0].tx_gain[i]);
      // Pair of the RX-antenna forcing above: keep TX on the transmit-capable connector, leaving
      // RX on its own. Same device-specific naming caveat -- see that block's comment.
      const char *tx_ant = NULL;
      if (device->type == USRP_B200_DEV) {
        tx_ant = "TX/RX"; // the only TX-capable port on a B2x0
      } else if (device->type == USRP_X400_DEV) {
        tx_ant = "TX/RX0"; // X410's TX-capable port (the bare "TX/RX" name does not exist there)
      }
      if (tx_ant != NULL) {
        s->usrp->set_tx_antenna(tx_ant, i + choffset);
        LOG_I(HW, "TX antenna forced to %s on channel %d\n",
              s->usrp->get_tx_antenna(i + choffset).c_str(), i);
      } else {
        LOG_I(HW, "TX antenna left at UHD default (%s) on channel %d -- port naming not verified "
                  "for this device type\n",
              s->usrp->get_tx_antenna(i + choffset).c_str(), i);
      }
    }
  }

  if (args.find("clock_source") == std::string::npos) {
    if (openair0_cfg[0].clock_source == internal) {
      s->usrp->set_clock_source("internal");
      LOG_I(HW, "Setting clock source to internal\n");
    } else if (openair0_cfg[0].clock_source == external) {
      s->usrp->set_clock_source("external");
      LOG_I(HW, "Setting clock source to external\n");
    } else if (openair0_cfg[0].clock_source == gpsdo) {
      s->usrp->set_clock_source("gpsdo");
      LOG_I(HW, "Setting clock source to gpsdo\n");
    } else {
      LOG_W(HW, "Clock source set neither in usrp_args nor on command line, using default!\n");
    }
  } else {
    if (openair0_cfg[0].clock_source != unset) {
      LOG_W(HW, "Clock source set in both usrp_args and in clock_source, ingnoring the latter!\n");
    }
  }

  if (args.find("time_source") == std::string::npos) {
    if (openair0_cfg[0].time_source == internal) {
      s->usrp->set_time_source("internal");
      LOG_I(HW, "Setting time source to internal\n");
    } else if (openair0_cfg[0].time_source == external) {
      s->usrp->set_time_source("external");
      LOG_I(HW, "Setting time source to external\n");
    } else if (openair0_cfg[0].time_source == gpsdo) {
      s->usrp->set_time_source("gpsdo");
      LOG_I(HW, "Setting time source to gpsdo\n");
    } else {
      LOG_W(HW, "Time source set neither in usrp_args nor on command line, using default!\n");
    }
  } else {
    if (openair0_cfg[0].time_source != unset) {
      LOG_W(HW, "Time source set in both usrp_args and in openair0_cfg[0].time_source, ignoring the latter!\n");
    }
  }

  if (s->usrp->get_clock_source(0) == "gpsdo") {
    s->use_gps = 1;

    if (sync_to_gps(device) == EXIT_SUCCESS) {
      LOG_I(HW, "USRP synced with GPS!\n");
    } else {
      LOG_I(HW, "USRP fails to sync with GPS. Exiting.\n");
      exit(EXIT_FAILURE);
    }
  } else {
    if (s->usrp->get_time_source(0) == "external") {
      usrp_sync_pps(s);
    } else {
      s->usrp->set_time_next_pps(uhd::time_spec_t(0.0));
    }

    if (s->usrp->get_clock_source(0) == "external") {
      if (check_ref_locked(s, 0)) {
        LOG_I(HW, "USRP locked to external reference!\n");
      } else {
        LOG_I(HW, "Failed to lock to external reference. Exiting.\n");
        exit(EXIT_FAILURE);
      }
    }
  }

  // display USRP settings
  LOG_I(HW,"Actual master clock: %fMHz...\n",s->usrp->get_master_clock_rate()/1e6);
  LOG_I(HW,"Actual clock source %s...\n",s->usrp->get_clock_source(0).c_str());
  LOG_I(HW,"Actual time source %s...\n",s->usrp->get_time_source(0).c_str());

  // create tx & rx streamer
  uhd::stream_args_t stream_args_rx("sc16", "sc16");
  for (int i = 0; i<openair0_cfg[0].rx_num_channels; i++) {
    LOG_I(HW,"setting rx channel %d\n",i+choffset);
    stream_args_rx.channels.push_back(i+choffset);
  }
  s->rx_stream = s->usrp->get_rx_stream(stream_args_rx);

  int samples=openair0_cfg[0].sample_rate;
  int max=s->rx_stream->get_max_num_samps();
  samples/=10000;
  LOG_I(HW,"RF board max packet size %u, size for 100µs jitter %d \n", max, samples);

  if ( samples < max ) {
    stream_args_rx.args["spp"] = str(boost::format("%d") % samples );
  }

  LOG_I(HW,"rx_max_num_samps %zu\n",
        s->rx_stream->get_max_num_samps());

  uhd::stream_args_t stream_args_tx("sc16", "sc16");

  for (int i = 0; i<openair0_cfg[0].tx_num_channels; i++)
    stream_args_tx.channels.push_back(i+choffset);

  s->tx_stream = s->usrp->get_tx_stream(stream_args_tx);

  /* Setting TX/RX BW after streamers are created due to USRP calibration issue */
  // N310 with UHD >= 4.2.0 has issues with changing the BW, which is a NOP on N310 in earlier versions
  // see also: https://github.com/EttusResearch/uhd/issues/644
  if (device->type != USRP_N300_DEV) {
    for(int i=0; i<((int) s->usrp->get_tx_num_channels()) && i<openair0_cfg[0].tx_num_channels; i++)
      s->usrp->set_tx_bandwidth(openair0_cfg[0].tx_bw,i+choffset);

    for(int i=0; i<((int) s->usrp->get_rx_num_channels()) && i<openair0_cfg[0].rx_num_channels; i++)
      s->usrp->set_rx_bandwidth(openair0_cfg[0].rx_bw,i+choffset);
  }

  for (int i=0; i<openair0_cfg[0].rx_num_channels; i++) {
    LOG_I(HW,"RX Channel %d\n",i);
    LOG_I(HW,"  Actual RX sample rate: %fMSps...\n",s->usrp->get_rx_rate(i+choffset)/1e6);
    LOG_I(HW,"  Actual RX frequency: %fGHz...\n", s->usrp->get_rx_freq(i+choffset)/1e9);
    LOG_I(HW,"  Actual RX gain: %f...\n", s->usrp->get_rx_gain(i+choffset));
    LOG_I(HW,"  Actual RX bandwidth: %fM...\n", s->usrp->get_rx_bandwidth(i+choffset)/1e6);
    LOG_I(HW,"  Actual RX antenna: %s...\n", s->usrp->get_rx_antenna(i+choffset).c_str());
  }

  for (int i=0; i<openair0_cfg[0].tx_num_channels; i++) {
    LOG_I(HW,"TX Channel %d\n",i);
    LOG_I(HW,"  Actual TX sample rate: %fMSps...\n", s->usrp->get_tx_rate(i+choffset)/1e6);
    LOG_I(HW,"  Actual TX frequency: %fGHz...\n", s->usrp->get_tx_freq(i+choffset)/1e9);
    LOG_I(HW,"  Actual TX gain: %f...\n", s->usrp->get_tx_gain(i+choffset));
    LOG_I(HW,"  Actual TX bandwidth: %fM...\n", s->usrp->get_tx_bandwidth(i+choffset)/1e6);
    LOG_I(HW,"  Actual TX antenna: %s...\n", s->usrp->get_tx_antenna(i+choffset).c_str());
    LOG_I(HW,"  Actual TX packet size: %lu\n",s->tx_stream->get_max_num_samps());
  }

  std::cout << boost::format("Using Device: %s") % s->usrp->get_pp_string() << std::endl;
  LOG_I(HW,"Device timestamp: %f...\n", s->usrp->get_time_now().get_real_secs());
  device->trx_write_func = trx_usrp_write;
  device->trx_read_func  = trx_usrp_read;
  s->sample_rate = openair0_cfg[0].sample_rate;

  // TODO:
  // init tx_forward_nsamps based usrp_time_offset ex
  if(is_equal(s->sample_rate, (double)30.72e6))
    s->tx_forward_nsamps  = 176;

  if(is_equal(s->sample_rate, (double)15.36e6))
    s->tx_forward_nsamps = 90;

  if(is_equal(s->sample_rate, (double)7.68e6))
    s->tx_forward_nsamps = 50;

  recplay_state_t *recPlay=device->recplay_state;

  if (recPlay != NULL) { // record mode
    recPlay->maxSizeBytes=openair0_cfg[0].recplay_conf->u_sf_max *
                            (sizeof(iqrec_t)+BELL_LABS_IQ_BYTES_PER_SF);
    recPlay->ms_sample = (iqrec_t *) malloc(recPlay->maxSizeBytes);
    recPlay->currentPtr= (uint8_t *)recPlay->ms_sample;

    if (recPlay->ms_sample == NULL) {
      std::cerr<< "Memory allocation failed for subframe record or replay mode." << std::endl;
      exit(-1);
    }
  }
  return 0;
}
/*@}*/
}/* extern c */
